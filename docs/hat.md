# HAT interface: serial line and PS/2 keyboard

What a VeeTee HAT plugs into, and which header pins carry what. This is
the contract between the board and the firmware. **The firmware for both
is not written yet**: today the line is USB CDC and keyboards are USB.
When it is, it follows this file. If a pin has to move, change this
file first.

## The header

The Waveshare RP2350-PiZero has a 40-pin header in the Raspberry Pi
layout, carrying GPIO 0–27. Everything here was checked against the
`40Pin OUT` block (connector J5) of Waveshare's schematic,
`files.waveshare.com/wiki/RP2350-PiZero/RP2350-PiZero.pdf`. Each
`GPIOn` net on J5 runs to the RP2350B's own `GPIOn` pin (U3: GPIO0 is
chip pin 77, GPIO4 is pin 1, and so on), so every GPIO number in this
file is the chip's own, the one the firmware uses.

**It is not BCM-numbered everywhere.** Twenty of the GPIOs sit where the
same BCM number would on a Pi. Waveshare moved the other eight so that
each pin's function matches a Pi instead: UART TX/RX on pins 8/10,
I²C on 3/5, and SPI on 19/21/23. The moved pins:

| Header pin | On a Pi (BCM) | On this board |
|---|---|---|
| 7 | GPIO 4 | GPIO 14 |
| 8 | GPIO 14 (TXD) | GPIO 4 (UART1 TX) |
| 10 | GPIO 15 (RXD) | GPIO 5 (UART1 RX) |
| 19 | GPIO 10 (MOSI) | GPIO 11 (SPI1 TX) |
| 21 | GPIO 9 (MISO) | GPIO 12 (SPI1 RX) |
| 23 | GPIO 11 (SCLK) | GPIO 10 (SPI1 SCK) |
| 29 | GPIO 5 | GPIO 15 |
| 32 | GPIO 12 | GPIO 9 |

Use the board's own map (under "Pin assignment" below), not a Pi's,
when you place the HAT's footprint.

None of GPIO 0–27 is used by the firmware today. Everything the board
already uses is off the header:

| GPIO | Used by |
|---|---|
| 28, 29 | PIO-USB keyboard port (D+, D-) |
| 30, 31, 40, 43 | SD card, SPI1 (SCK, MOSI, MISO, CS) |
| 32–39 | DVI TMDS |
| 44, 45, 46 | HDMI DDC and CEC (wired to the HDMI connector, not used) |

All logic on the header is **3.3 V**. Pins 2 and 4 are **VBUS**: the
5 V straight from the USB-C port, with no regulator or switch between
them.

## Constraints that set the pin choice

- **Hardware UARTs.** Each GPIO can only reach one of the two UARTs.
  On the header, UART0 is on GPIO 0–3 and 12–19, and UART1 is on 4–11
  and 20–27. Within each group of four starting at a multiple of 4, the
  pins are TX, RX, CTS, RTS in that order (function F2). The RP2350 also
  lets the CTS/RTS pins act as TX/RX instead (function F11, `UART_AUX`).
  This matches arduino-pico's `SerialUART.cpp` pin tables for the
  RP2350B.
- **PIO.** pio0 drives DVI and has been shifted to see GPIO 16–47
  (`pio_set_gpio_base(pio0, 16)`), so **it cannot reach GPIO 0–15**.
  DVI uses state machines 0–2, which leaves state machine 3 free for
  PS/2. pio1 and pio2 belong to PIO-USB: it uses state machines in both,
  and its two programs already collided once over a PIO interrupt flag
  (`lib/hal/usbhost.c`). Anything PIO-driven on the HAT therefore goes on
  **GPIO 16–27**, and anything else goes below 16 to keep that range free.
- **Pull resistors.** RP2350 erratum E9 (A2 silicon) means an input
  held by the internal pull-down can latch at about 2 V. Every input
  below is either driven by a chip or pulled up externally, so it isn't
  a problem here. Don't design anything that relies on the internal
  pull-downs.

## Pin assignment

| Function | GPIO | Header pin | Direction (from the VeeTee) | Peripheral |
|---|---|---|---|---|
| Serial TXD | 4 | 8 | out | UART1 TX (F2) |
| Serial RXD | 5 | 10 | in | UART1 RX (F2) |
| Serial CTS | 6 | 31 | in | UART1 CTS (F2) |
| Serial RTS | 7 | 26 | out | UART1 RTS (F2) |
| PS/2 DATA | 16 | 36 | open-drain, both ways | pio0 SM 3 |
| PS/2 CLK | 17 | 11 | open-drain, both ways | pio0 SM 3 |
| DTR (optional) | 22 | 15 | out | plain GPIO |
| DCD (optional) | 23 | 16 | in | plain GPIO |
| DSR (optional) | 24 | 18 | in | plain GPIO |
| RI (optional) | 25 | 22 | in | plain GPIO |

The whole header, from the schematic. **Bold** marks the pins that
differ from a Pi's BCM layout:

| Use | Signal | Pin | Pin | Signal | Use |
|---|---|---|---|---|---|
| | 3.3 V | 1 | 2 | VBUS (5 V) | PS/2 power |
| | GPIO 2 | 3 | 4 | VBUS (5 V) | |
| | GPIO 3 | 5 | 6 | GND | |
| | **GPIO 14** | 7 | 8 | **GPIO 4** | Serial TXD |
| | GND | 9 | 10 | **GPIO 5** | Serial RXD |
| PS/2 CLK | GPIO 17 | 11 | 12 | GPIO 18 | |
| | GPIO 27 | 13 | 14 | GND | |
| DTR | GPIO 22 | 15 | 16 | GPIO 23 | DCD |
| | 3.3 V | 17 | 18 | GPIO 24 | DSR |
| | **GPIO 11** | 19 | 20 | GND | |
| | **GPIO 12** | 21 | 22 | GPIO 25 | RI |
| | **GPIO 10** | 23 | 24 | GPIO 8 | |
| | GND | 25 | 26 | GPIO 7 | Serial RTS |
| | GPIO 0 | 27 | 28 | GPIO 1 | |
| | **GPIO 15** | 29 | 30 | GND | |
| Serial CTS | GPIO 6 | 31 | 32 | **GPIO 9** | |
| | GPIO 13 | 33 | 34 | GND | |
| | GPIO 19 | 35 | 36 | GPIO 16 | PS/2 DATA |
| | GPIO 26 | 37 | 38 | GPIO 20 | |
| | GND | 39 | 40 | GPIO 21 | |

Why these pins:

- **UART1 on GPIO 4–7.** These four make up one complete UART1 block
  on the plain UART function, and they're below 16, so they use none of
  the pins pio0 can reach. Waveshare wired GPIO 4/5 to pins 8/10, so
  TXD/RXD are where a Pi's serial port has them. CTS/RTS (pins 31/26)
  don't follow any Pi convention.
- **GPIO 0/1 (pins 27/28) are left alone.** They're reserved for the ID
  EEPROM on Pi HATs, and the schematic labels them ID_SDA/ID_SCL.
- **PS/2 DATA then CLK, next to each other.** pio0 can reach both pins,
  and having DATA first with CLK right after keeps the PIO program
  simple.
- **The modem lines are optional.** They're plain GPIOs, read and set by
  the firmware from time to time. If you leave them off the HAT, GPIO
  22–25 stay free.

Free for anything else afterwards: GPIO 0–3 and 8–15, 18–21, 26 and 27,
plus 22–25 if you skip the modem lines. A second serial port would go
on UART0, GPIO 12–15: TXD pin 21, RXD 33, CTS 7, RTS 29.

## Serial port (RS-232)

The header carries 3.3 V TTL serial. For a real serial port, add a
3.3 V RS-232 transceiver and a **DE-9 male** connector wired as DTE (the
VeeTee is the terminal):

| DE-9 pin | Signal | Direction | From GPIO (header pin) |
|---|---|---|---|
| 1 | DCD | in | 23 (16), optional |
| 2 | RXD | in | 5 (10) |
| 3 | TXD | out | 4 (8) |
| 4 | DTR | out | 22 (15), optional |
| 5 | GND | | GND |
| 6 | DSR | in | 24 (18), optional |
| 7 | RTS | out | 7 (26) |
| 8 | CTS | in | 6 (31) |
| 9 | RI | in | 25 (22), optional |

For TXD, RXD, RTS and CTS only, a MAX3232-class part is enough (2
drivers, 2 receivers). For the full DTE set, use a MAX3243-class part
(3 drivers, 5 receivers: TXD/RTS/DTR out, RXD/CTS/DSR/DCD/RI in). Check
the datasheet of whichever part you choose. The transceiver does the
RS-232 inversion, so the UART runs at normal TTL polarity.

A header for 3.3 V TTL straight off GPIO 4–7 (GND, TXD, RXD, CTS, RTS),
placed before the transceiver or with jumpers to bypass it, is cheap and
useful for testing with a USB-TTL adapter.

## PS/2 keyboard

Use a **6-pin mini-DIN female** connector (the host side). Pin numbers
follow the standard PS/2 numbering, which KiCad's Mini-DIN-6 footprints
use too:

| Mini-DIN pin | Signal |
|---|---|
| 1 | DATA |
| 2 | not connected |
| 3 | GND |
| 4 | +5 V |
| 5 | CLK |
| 6 | not connected |

PS/2 is 5 V and open-collector on both lines. The VeeTee pulls CLK low
to hold the keyboard off and to send it commands (the Caps/Num/Scroll
Lock LEDs), so both lines have to work in both directions:

- **Level shifting: one BSS138 MOSFET per line**, as in the common
  bidirectional I²C level-shifter circuit. PS/2 is open-drain like I²C,
  so the same circuit works. Put pull-ups on both sides of each FET:
  10 kΩ to 3.3 V on the GPIO side, and 4.7–10 kΩ to 5 V on the keyboard
  side (these are the host pull-ups PS/2 expects).
- **Don't rely on the RP2350 tolerating 5 V.** The pads may well survive
  it, but it hasn't been checked against the datasheet, and the shifter
  costs pennies.
- **Power.** Feed the keyboard's 5 V from header pin 2 or 4 (VBUS)
  through a polyfuse of a few hundred mA, with a bulk capacitor (about
  10 µF) after it. PS/2 isn't designed to be hot-plugged. The board's
  PIO-USB port already browns out when a device is plugged in while it's
  running (`docs/hardware.md`), and the keyboard draws from the same
  VBUS.

How the firmware will read it: a PIO program on pio0 state machine 3
captures scancode set 2 frames into its FIFO (11 bits: start, 8 data,
odd parity, stop). The PIO keeps capturing while the CPU is busy:
rendering takes about 42% of core 0 from a top-priority interrupt, and
interrupts are off while settings are saved to flash. A GPIO interrupt
would miss bits during either. The bits are only valid for 30–50 µs.
Decoded scancodes become the same key events as the USB keyboard's.
