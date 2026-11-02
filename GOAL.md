# VeeTee 2350: An rp2350-based terminal emulator.

Your goal is to build a serial terminal that outputs via DVI (HDMI) based on the RP2350.

Requirements:

* Built-in configuration
* HID keyboard support
* Support for standard terminal escape codes
* Support for DEC reGIS, Sixel graphics commands
* Supporrt for emulation of the VT100 through to more modern terminals. 
* Support for loadable fonts
* Support for 24-bit color (xterm-color)

Stretch goals:
* Unicode (limited) support.
* SD card support (screenshot, recording/playback, etc.)

Goal 1:

* Emulate a VT100.
* Read from the built-in USB UART on the RP2350 for input
* "loopback" on a secondary USB UART that emulates the keyboard

Goal 2:

* Graphics support
* DEC-specific control codes (e.g. VT100 keyboard lights) on-display.

Goal 3: 

* USB HID keyboard support

Later:

* Support for built-in UART serial channels (needs hardware I don't have on hand) 

# Your environment

You have a waveshare rp2350-PiZero (https://www.waveshare.com/wiki/RP2350-PiZero) and a USB HDMI framegrabber, a Genki streaming adapter.

Build at least goal 1. 

# Tools

Use platformio for development. You should work in the latest C standard that the compiler supports. Write clean, concice C. Build an abstraction.

# Benchmarks

* [VTTest](https://invisible-island.net/vttest/)
* [Terminal Tests](https://github.com/japhb/Terminal-Tests)
* [ESCTest](https://github.com/ThomasDickey/esctest2)

Use these as goals, not hard measures. ESCTest and VTTest are the most appropriate for this situation. 
