# HAT interface: serial line and PS/2 keyboard

What a VeeTee HAT plugs into, and which header pins carry what. This is
the contract between the board and the firmware. **The firmware for both
is not written yet**: today the line is USB CDC and keyboards are USB.
When it is, it follows this file. If a pin has to move, change this
file first.

## The header

The Waveshare RP2350-PiZero has a 40-pin header that Waveshare calls
"compatible with Raspberry Pi". It exposes 28 GPIOs, GPIO 0–27, and they
are assumed to sit at the same positions as BCM GPIO 0–27 on a Pi.
**Check that against Waveshare's schematic
(`files.waveshare.com/wiki/RP2350-PiZero/RP2350-PiZero.pdf`) before you
order boards.** The wiki confirms the header and the GPIO count, but not
each pin's position.

None of GPIO 0–27 is used by the firmware today. Everything the board
already uses is off the header:

| GPIO | Used by |
|---|---|
| 28, 29 | PIO-USB keyboard port (D+, D-) |
| 30, 31, 40, 43 | SD card, SPI1 (SCK, MOSI, MISO, CS) |
| 32–39 | DVI TMDS |
| 44, 45, 46 | HDMI DDC and CEC (wired to the HDMI connector, not used) |

All logic on the header is **3.3 V**.

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
| Serial TXD | 12 | 32 | out | UART0 TX (F2) |
| Serial RXD | 13 | 33 | in | UART0 RX (F2) |
| Serial CTS | 14 | 8 | in | UART0 CTS (F2) |
| Serial RTS | 15 | 10 | out | UART0 RTS (F2) |
| PS/2 DATA | 16 | 36 | open-drain, both ways | pio0 SM 3 |
| PS/2 CLK | 17 | 11 | open-drain, both ways | pio0 SM 3 |
| DTR (optional) | 22 | 15 | out | plain GPIO |
| DCD (optional) | 23 | 16 | in | plain GPIO |
| DSR (optional) | 24 | 18 | in | plain GPIO |
| RI (optional) | 25 | 22 | in | plain GPIO |

The whole header, with Pi pin numbering and the BCM positions assumed
above:

| Use | Signal | Pin | Pin | Signal | Use |
|---|---|---|---|---|---|
| | 3.3 V | 1 | 2 | 5 V | PS/2 power |
| | GPIO 2 | 3 | 4 | 5 V | |
| | GPIO 3 | 5 | 6 | GND | |
| | GPIO 4 | 7 | 8 | GPIO 14 | Serial CTS |
| | GND | 9 | 10 | GPIO 15 | Serial RTS |
| PS/2 CLK | GPIO 17 | 11 | 12 | GPIO 18 | |
| | GPIO 27 | 13 | 14 | GND | |
| DTR | GPIO 22 | 15 | 16 | GPIO 23 | DCD |
| | 3.3 V | 17 | 18 | GPIO 24 | DSR |
| | GPIO 10 | 19 | 20 | GND | |
| | GPIO 9 | 21 | 22 | GPIO 25 | RI |
| | GPIO 11 | 23 | 24 | GPIO 8 | |
| | GND | 25 | 26 | GPIO 7 | |
| | GPIO 0 | 27 | 28 | GPIO 1 | |
| | GPIO 5 | 29 | 30 | GND | |
| | GPIO 6 | 31 | 32 | GPIO 12 | Serial TXD |
| Serial RXD | GPIO 13 | 33 | 34 | GND | |
| | GPIO 19 | 35 | 36 | GPIO 16 | PS/2 DATA |
| | GPIO 26 | 37 | 38 | GPIO 20 | |
| | GND | 39 | 40 | GPIO 21 | |

Why these pins:

- **UART on GPIO 12–15.** These four make up one complete UART0 block
  on the plain UART function. They're below 16, so they use none of the
  pins pio0 can reach. GPIO 0–3 would also work, but GPIO 0/1 (pins
  27/28) are reserved for the ID EEPROM on Pi HATs, so this layout
  avoids them.
- **Different from a Pi's serial layout.** A Pi serial HAT has TXD/RXD
  on pins 8/10 (GPIO 14/15). Here those pins are CTS/RTS, so an
  off-the-shelf Pi serial HAT won't work as-is. A Pi-compatible layout
  is possible: TXD/RXD on 14/15 via F11, with CTS/RTS on 18/19. It's
  untested, though, and it would take 18/19 away from PIO.
- **PS/2 DATA then CLK, next to each other.** pio0 can reach both pins,
  and having DATA first with CLK right after keeps the PIO program
  simple.
- **The modem lines are optional.** They're plain GPIOs, read and set by
  the firmware from time to time. If you leave them off the HAT, GPIO
  22–25 stay free.

Free for anything else afterwards: GPIO 0–11 (UART1 is on 4–7 or 8–11
if you want a second port), 18–21, 26 and 27, plus 22–25 if you skip the
modem lines.

## Serial port (RS-232)

The header carries 3.3 V TTL serial. For a real serial port, add a
3.3 V RS-232 transceiver and a **DE-9 male** connector wired as DTE (the
VeeTee is the terminal):

| DE-9 pin | Signal | Direction | From GPIO |
|---|---|---|---|
| 1 | DCD | in | 23 (optional) |
| 2 | RXD | in | 13 |
| 3 | TXD | out | 12 |
| 4 | DTR | out | 22 (optional) |
| 5 | GND | | GND |
| 6 | DSR | in | 24 (optional) |
| 7 | RTS | out | 15 |
| 8 | CTS | in | 14 |
| 9 | RI | in | 25 (optional) |

For TXD, RXD, RTS and CTS only, a MAX3232-class part is enough (2
drivers, 2 receivers). For the full DTE set, use a MAX3243-class part
(3 drivers, 5 receivers: TXD/RTS/DTR out, RXD/CTS/DSR/DCD/RI in). Check
the datasheet of whichever part you choose. The transceiver does the
RS-232 inversion, so the UART runs at normal TTL polarity.

A header for 3.3 V TTL straight off GPIO 12–15 (GND, TXD, RXD, CTS,
RTS), placed before the transceiver or with jumpers to bypass it, is
cheap and useful for testing with a USB-TTL adapter.

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
- **Power.** Feed the keyboard's 5 V from the header through a polyfuse
  of a few hundred mA. PS/2 isn't designed to be hot-plugged. The board's
  PIO-USB port browns out when a device is plugged in while it's running
  (`docs/hardware.md`), and plugging in a keyboard draws from the same
  5 V. Put a bulk capacitor (about 10 µF) on the keyboard's 5 V.

How the firmware will read it: a PIO program on pio0 state machine 3
captures scancode set 2 frames into its FIFO (11 bits: start, 8 data,
odd parity, stop). The PIO keeps capturing while the CPU is busy:
rendering takes about 42% of core 0 from a top-priority interrupt, and
interrupts are off while settings are saved to flash. A GPIO interrupt
would miss bits during either. The bits are only valid for 30–50 µs.
Decoded scancodes become the same key events as the USB keyboard's.
