/*
 * video.c — DVI scanout on the Waveshare RP2350-PiZero.
 *
 * Built on vendored libdvi (lib/PicoDVI). Work is split across the cores:
 *
 *   core 0: scanline callback (the text renderer) -> RGB565 line, written
 *           into a small ring of line buffers from a doorbell IRQ
 *   core 1: takes lines from the ring in order -> SIO TMDS encoder, once
 *           per lane (B, G, R) -> buffer pushed onto dvi0.q_tmds_valid
 *   DMA:    streams the three lanes into three PIO state machines, which
 *           shift 10-bit symbols out as differential pairs; the TMDS clock
 *           pair is a PWM slice running at sysclk/10.
 *
 * Why split: one line is 8000 sysclk cycles. TMDS encode alone takes
 * ~4.6k of them, and the text renderer ~3k on a dense screen, so with
 * both on core 1 the worst lines overran (docs/hardware.md). Core 0 only
 * does bursty USB + escape parsing, so it has room. Rendering there in a
 * top-priority IRQ keeps it ahead of the encoder no matter what the
 * terminal code is doing, and leaves core 1 with ~40% headroom for future
 * work (graphics overlays for Sixel/ReGIS).
 *
 * The ring: RING_LINES buffers; core 0 produces (ring_head), core 1
 * consumes (ring_tail). After each line core 1 rings a doorbell; core 0's
 * IRQ then renders until the ring is full again. 8 lines = ~254 us of
 * slack, which is how long core 0 may be held off (another IRQ, a
 * critical section) before the picture glitches.
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
#include "hardware/irq.h"
#include "hardware/structs/busctrl.h"
#include "hardware/structs/m33.h"
#include "hardware/structs/systick.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico.h"
#include "pico/multicore.h"

/*
 * Guard against the build silently targeting the 30-GPIO RP2350A: then
 * PICO_PIO_USE_GPIO_BASE is 0, pins >= 32 alias to 0-15, and the screen
 * stays dark with no error anywhere.
 */
static_assert(NUM_BANK0_GPIOS == 48, "RP2350B (48 GPIO) build required");
static_assert(PICO_PIO_USE_GPIO_BASE, "PIO GPIO base support required");
static_assert(DVI_SYMBOLS_PER_WORD == 2, "lane stride below assumes 2 sym/word");
static_assert(DVI_VERTICAL_REPEAT == 1, "full-res rendering assumes no repeat");

#define RING_LINES	8	/* power of two: ring index is a mask */
static_assert((RING_LINES & (RING_LINES - 1)) == 0);

/*
 * The doorbell core 1 rings to ask core 0 for more lines. NOT doorbell 0:
 * arduino-pico in single-core mode never claims a doorbell, yet still
 * installs a shared SIO_IRQ_BELL handler watching doorbell 0 that, when
 * set, disables interrupts and spins forever ("idle other core"). Claiming
 * a fixed high doorbell keeps us clear of it.
 */
#define VIDEO_DOORBELL	7

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

/* Line ring. Producer: core 0 (IRQ). Consumer: core 1. */
static uint16_t ring[RING_LINES][VIDEO_WIDTH] __attribute__((aligned(4)));
static volatile uint32_t ring_head;	/* lines produced, written by core 0 */
static volatile uint32_t ring_tail;	/* lines consumed, written by core 1 */
static unsigned produce_line;		/* core 0 only: next scanline index */

/* Per-frame statistics; each field has a single writer core. */
static volatile struct video_stats live_stats;
static uint32_t render_frame_total;	/* core 0 accumulators */
static uint32_t render_frame_max;

/* --- cycle counters ---------------------------------------------------
 * Core 1 uses its SysTick (24-bit, counts down). Core 0 uses the DWT
 * cycle counter instead, since the Arduino core may own core 0's SysTick.
 * Both are core-local registers on the private peripheral bus.
 */
static inline void
systick_counter_start(void)
{
	systick_hw->rvr = 0x00ffffff;
	systick_hw->cvr = 0;
	systick_hw->csr = 0x5;		/* CLKSOURCE = processor, ENABLE */
}

static inline uint32_t
systick_counter_now(void)
{
	return systick_hw->cvr;
}

static inline void
dwt_counter_start(void)
{
	m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
	m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
}

/* --- core 0: producer ------------------------------------------------ */

/*
 * Render lines until the ring is full. Called from the doorbell IRQ, and
 * once synchronously at init to prime the ring before scanout starts.
 */
static void __not_in_flash_func(produce_lines)(void)
{
	while (ring_head - ring_tail < RING_LINES) {
		uint32_t start = m33_hw->dwt_cyccnt;
		scanline_fn(ring[ring_head & (RING_LINES - 1)], produce_line,
		    scanline_ctx);
		uint32_t spent = m33_hw->dwt_cyccnt - start;

		/* Pixels must be visible to core 1 before the index is. */
		__dmb();
		ring_head = ring_head + 1;

		render_frame_total += spent;
		if (spent > render_frame_max)
			render_frame_max = spent;
		if (++produce_line == VIDEO_HEIGHT) {
			produce_line = 0;
			live_stats.avg_render_cycles = render_frame_total /
			    VIDEO_HEIGHT;
			live_stats.max_render_cycles = render_frame_max;
			render_frame_total = 0;
			render_frame_max = 0;
		}
	}
}

/*
 * Shared SIO_IRQ_BELL handler (all doorbells share one IRQ line); only act
 * on ours. Clear before producing, so a ring that arrives mid-loop simply
 * re-raises the IRQ instead of being lost.
 */
static void __not_in_flash_func(doorbell_irq)(void)
{
	if (!multicore_doorbell_is_set_current_core(VIDEO_DOORBELL))
		return;
	multicore_doorbell_clear_current_core(VIDEO_DOORBELL);
	produce_lines();
}

/* --- core 1: consumer ------------------------------------------------ */

/*
 * Encode an RGB565 line into three TMDS lanes. Channel bit ranges match
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

static void __not_in_flash_func(encode_line)(const uint16_t *pixels,
    uint32_t *tmds)
{
	const uint32_t *pix = (const uint32_t *)pixels;

	tmds_encode_data_channel_fullres_16bpp(pix, tmds + 0 * LANE_WORDS,
	    VIDEO_WIDTH, 4, 0);
	tmds_encode_data_channel_fullres_16bpp(pix, tmds + 1 * LANE_WORDS,
	    VIDEO_WIDTH, 10, 5);
	tmds_encode_data_channel_fullres_16bpp(pix, tmds + 2 * LANE_WORDS,
	    VIDEO_WIDTH, 15, 11);
}

/*
 * Core 1 entry. Owns the DVI DMA IRQ and encodes lines in display order
 * forever. libdvi starts scanout in vertical blanking; if a line is ever
 * late, libdvi emits filler and resyncs, so the picture tears for a frame
 * but never loses vertical alignment. `starved` counts lines where the
 * ring was empty when core 1 wanted one -- the early warning that core 0
 * is falling behind.
 */
static void __not_in_flash_func(core1_scanout)(void)
{
	dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
	systick_counter_start();
	dvi_start(&dvi0);

	for (;;) {
		uint32_t frame_max = 0;
		uint32_t frame_total = 0;

		for (unsigned line = 0; line < VIDEO_HEIGHT; line++) {
			uint32_t *tmds;
			uint32_t seq = ring_tail;

			queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmds);
			if (ring_head == seq) {
				live_stats.starved = live_stats.starved + 1;
				while (ring_head == seq)
					tight_loop_contents();
			}
			__dmb();

			uint32_t start = systick_counter_now();
			encode_line(ring[seq & (RING_LINES - 1)], tmds);
			uint32_t spent = (start - systick_counter_now()) &
			    0x00ffffff;

			/* Done reading the slot: hand it back, wake core 0. */
			__dmb();
			ring_tail = seq + 1;
			multicore_doorbell_set_other_core(VIDEO_DOORBELL);

			queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmds);

			frame_total += spent;
			if (spent > frame_max)
				frame_max = spent;
		}

		live_stats.max_encode_cycles = frame_max;
		live_stats.avg_encode_cycles = frame_total / VIDEO_HEIGHT;
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

	/* Prime the ring, then let core 1's doorbells drive core 0. */
	dwt_counter_start();
	produce_lines();
	multicore_doorbell_claim(VIDEO_DOORBELL, 0b11);
	uint bell_irq = multicore_doorbell_irq_num(VIDEO_DOORBELL);
	irq_add_shared_handler(bell_irq, doorbell_irq,
	    PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);
	irq_set_priority(bell_irq, PICO_HIGHEST_IRQ_PRIORITY);
	irq_set_enabled(bell_irq, true);

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
		out->avg_encode_cycles = live_stats.avg_encode_cycles;
		out->max_encode_cycles = live_stats.max_encode_cycles;
		out->avg_render_cycles = live_stats.avg_render_cycles;
		out->max_render_cycles = live_stats.max_render_cycles;
		out->starved = live_stats.starved;
		__dmb();
	} while (frames != live_stats.frames);
	out->frames = frames;
}
