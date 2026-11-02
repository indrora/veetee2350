# 007 — A keyboard port, and making core 1 flash-free

*2026-11-01 · commit `69f87d0`*

Goal 1 asks for a second USB serial port that acts as the terminal's
keyboard. arduino-pico's native USB stack is compiled with exactly one
CDC interface, so we moved to **Adafruit TinyUSB**, which ships
configured for two. Now the board shows up as `VeeTee line` and
`VeeTee keyboard`. Keys written to the keyboard port reach the host on
the line port:

```
line port received: b'typed on the keyboard\r'
```

The SET-UP screen will use the BOOTSEL button as its key, and settings
need saving to flash. Both make flash unreadable for a while, so core 1
must not run anything from flash. A new tool, `tools/core1_audit.py`,
walks core 1's call graph in the firmware ELF, and it found three more
`-Os` out-linings into flash: libdvi's queue helpers, the SDK's
`queue_get_level_unsafe`, and `dma_channel_get_default_config`, which is
called **every scanline inside the DMA interrupt**. They're now
force-inlined or precomputed (PicoDVI `PATCHES.md` 3–4), and the audit
runs clean.
