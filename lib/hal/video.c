/*
 * video.c — DVI scanout on the Waveshare RP2350-PiZero.
 *
 * Built on vendored libdvi (lib/PicoDVI). The pipeline per visible line:
 *
 *   core 1: scanline callback -> 640 x RGB565 in line_pixels[]
 *           -> SIO TMDS encoder, once per lane (B, G, R)
 *           -> buffer pushed onto dvi0.q_tmds_valid
 *   DMA:    streams the three lanes into three PIO state machines, which
 *           shift 10-bit symbols out as differential pairs; the TMDS clock
 *           pair is a PWM slice running at sysclk/10.
 *
 * Board specifics (verified against two independent sources, see
 * docs/hardware.md): TMDS D0/D1/D2 on GPIO 36/34/32, clock on GPIO 38,
 * no pair inversion. GPIO >= 32 is only reachable by a PIO block whose
 * GPIO base is 16, so pio0 is rebased before dvi_init(). Consequence:
 * pio0 can no longer touch GPIO 0-15. Other PIO users (PIO-USB host in
 * Goal 3) must use pio1/pio2.
 */
#include "video.h"

#include <stdbool.h>

#include "dvi.h"
#include "dvi_serialiser.h"
#include "dvi_timing.h"
#include "tmds_encode.h"

#include "hardware/clocks.h"
#include "hardware/structs/busctrl.h"
#include "hardware/irq.h"
#include "hardware/structs/systick.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico.h"

/*
 * Guard against the build silently targeting the 30-GPIO RP2350A: then
 * PICO_PIO_USE_GPIO_BASE is 0, pins >= 32 alias to 0-15, and the screen
 * stays dark with no error anywhere.
 */
static_assert(NUM_BANK0_GPIOS == 48, "RP2350B (48 GPIO) build required");
static_assert(PICO_PIO_USE_GPIO_BASE, "PIO GPIO base support required");
static_assert(DVI_SYMBOLS_PER_WORD == 2, "lane stride below assumes 2 sym/word");
static_assert(DVI_VERTICAL_REPEAT == 1, "full-res rendering assumes no repeat");

static const struct dvi_serialiser_cfg pizero_dvi_cfg = {
	.pio = pio0,
	.sm_tmds = {0, 1, 2},
	.pins_tmds = {36, 34, 32},	/* D0 (blue), D1 (green), D2 (red) */
	.pins_clk = 38,
	.invert_diffpairs = false,
};

static struct dvi_inst dvi0;

static video_scanline_fn scanline_fn;
static void *scanline_ctx;

/* Written by core 1 only; core 0 reads via video_get_stats(). */
static volatile struct video_stats live_stats;

/* One line of RGB565, 4-byte aligned because the encoder reads words. */
static uint16_t line_pixels[VIDEO_WIDTH] __attribute__((aligned(4)));

/*
 * Core-local SysTick as a cycle counter. 24 bits wraps every ~66 ms at
 * 252 MHz, far longer than one line, so a single subtraction is enough.
 * It counts DOWN, hence start - end.
 */
static inline void
cycle_counter_start(void)
{
	systick_hw->rvr = 0x00ffffff;
	systick_hw->cvr = 0;
	systick_hw->csr = 0x5;		/* CLKSOURCE = processor, ENABLE */
}

static inline uint32_t
cycle_counter_now(void)
{
	return systick_hw->cvr;
}

/*
 * Encode the RGB565 line into three TMDS lanes. Channel bit ranges match
 * RGB565: blue 4:0, green 10:5, red 15:11. Lane order follows the
 * serialiser config (lane 0 = D0 = blue).
 *
 * Two 10-bit symbols are packed per FIFO word (DVI_SYMBOLS_PER_WORD=2), so
 * each lane is VIDEO_WIDTH/2 words. This matters for speed: the SIO TMDS
 * encoder can pop both symbols of a pixel pair in one read. Measured on
 * hardware with one symbol per word, encode alone cost ~9.2k cycles/line,
 * over the 8k budget.
 */
#define LANE_WORDS	(VIDEO_WIDTH / DVI_SYMBOLS_PER_WORD)

static void __not_in_flash_func(encode_line)(uint32_t *tmds)
{
	const uint32_t *pix = (const uint32_t *)line_pixels;

	tmds_encode_data_channel_fullres_16bpp(pix, tmds + 0 * LANE_WORDS,
	    VIDEO_WIDTH, 4, 0);
	tmds_encode_data_channel_fullres_16bpp(pix, tmds + 1 * LANE_WORDS,
	    VIDEO_WIDTH, 10, 5);
	tmds_encode_data_channel_fullres_16bpp(pix, tmds + 2 * LANE_WORDS,
	    VIDEO_WIDTH, 15, 11);
}

/*
 * Core 1 entry. Owns the DVI DMA IRQ and produces lines in display order
 * forever. libdvi starts scanout in vertical blanking, which gives us the
 * whole blanking interval to render line 0; if we ever fall behind, libdvi
 * emits filler lines and then discards our late buffers to resync, so the
 * picture tears for a frame but never loses vertical alignment.
 */
static void __not_in_flash_func(core1_scanout)(void)
{
	dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
	cycle_counter_start();
	dvi_start(&dvi0);

	for (;;) {
		uint32_t frame_max = 0;
		uint32_t frame_total = 0;
		uint32_t frame_encode = 0;

		for (unsigned line = 0; line < VIDEO_HEIGHT; line++) {
			uint32_t *tmds;

			queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmds);

			uint32_t start = cycle_counter_now();
			scanline_fn(line_pixels, line, scanline_ctx);
			uint32_t rendered = cycle_counter_now();
			encode_line(tmds);
			uint32_t spent = (start - cycle_counter_now()) &
			    0x00ffffff;

			queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmds);

			frame_total += spent;
			frame_encode += (rendered - cycle_counter_now()) &
			    0x00ffffff;
			if (spent > frame_max)
				frame_max = spent;
		}

		live_stats.max_line_cycles = frame_max;
		live_stats.avg_line_cycles = frame_total / VIDEO_HEIGHT;
		live_stats.avg_encode_cycles = frame_encode / VIDEO_HEIGHT;
		__dmb();
		live_stats.frames = live_stats.frames + 1;
	}
}

void
video_init(video_scanline_fn scanline, void *ctx)
{
	scanline_fn = scanline;
	scanline_ctx = ctx;

	/*
	 * arduino-pico already runs sysclk at F_CPU (platformio.ini) but only
	 * raises VREG to 1.15 V. PicoDVI's tested setting for 252 MHz is
	 * 1.20 V; bump it, let it settle, then make sure the clock is exactly
	 * the TMDS bit rate (25.2 MHz pixel clock x 10).
	 */
	vreg_set_voltage(VREG_VOLTAGE_1_20);
	busy_wait_ms(10);
	if (clock_get_hz(clk_sys) != VIDEO_SYSCLK_KHZ * 1000u &&
	    !set_sys_clock_khz(VIDEO_SYSCLK_KHZ, false))
		panic("video: cannot reach %u kHz sysclk", VIDEO_SYSCLK_KHZ);

	dvi0.timing = &dvi_timing_640x480p_60hz;
	dvi0.ser_cfg = pizero_dvi_cfg;
	if (pio_set_gpio_base(dvi0.ser_cfg.pio, 16) != PICO_OK)
		panic("video: cannot rebase PIO to GPIO 16-47");
	dvi_init(&dvi0, next_striped_spin_lock_num(),
	    next_striped_spin_lock_num());

	/*
	 * Core 1's loads/stores race the three DVI DMA channels for SRAM. Give
	 * the processor priority on the bus fabric (as PicoDVI's own full-res
	 * demos do); DMA has plenty of slack within a line.
	 */
	hw_set_bits(&busctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
	multicore_launch_core1(core1_scanout);
}

void
video_get_stats(struct video_stats *out)
{
	uint32_t frames;

	/* Re-read until a frame boundary didn't land mid-copy. */
	do {
		frames = live_stats.frames;
		__dmb();
		out->max_line_cycles = live_stats.max_line_cycles;
		out->avg_line_cycles = live_stats.avg_line_cycles;
		out->avg_encode_cycles = live_stats.avg_encode_cycles;
		__dmb();
	} while (frames != live_stats.frames);
	out->frames = frames;
}
