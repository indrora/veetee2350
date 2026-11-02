# Local patches to pico_pio_usb

Changes to the vendored Pico-PIO-USB 0.5.2 (`PROVENANCE.md`), each
marked `VeeTee patch N` in the source.

## 1. Three host calls TinyUSB 3.7.7 needs (`src/pio_usb_host.c`, `src/pio_usb_ll.h`)

Adafruit TinyUSB 3.7.7's `hcd_pio_usb.c` calls
`pio_usb_host_get_frame_number`, `pio_usb_host_endpoint_close` and
`pio_usb_host_endpoint_abort_transfer`, which upstream added after 0.5.2.
Here: a frame counter bumped in `sof_timer` (one frame a millisecond);
close clears the endpoint's `has_transfer` then its `size` (the pool's
"in use" mark, which `pio_usb_host_endpoint_open` and `_find_ep` test);
abort clears `has_transfer` if a transfer was pending. Written from 0.5.2's
own structures, not copied from a later release.

## 2. Bus counters (`src/pio_usb_ll.h`, `src/pio_usb.c`, `src/pio_usb_host.c`)

`pio_usb_veetee_stats`: connections seen, port resets, transfers
completed / failed / stalled, NAKs, and IN transactions that got no
reply at all. A few increments a frame; the firmware's debug builds print
them once a second (`[usb]`), which is how the first bring-up was
diagnosed (devlog 027).

## 3. Any PIO block (`src/pio_usb.c`)

0.5.2 picked a block with `num == 0 ? pio0 : pio1`, so PIO 2 couldn't be
had. Now `pio_get_instance(num)`. It matters because transmit and receive
can't share a block: the transmit program's end-of-packet flag is PIO
interrupt flag 1, and the receive program's bit-stuff error is flag 1
too, so in one block every packet sent looked like a receive error (the
first bring-up: four failed SETUPs, devlog 027). With DVI on PIO 0,
transmit has PIO 1 and receive PIO 2. (The device side's IRQ choice in
`pio_usb_device.c` still assumes 0 or 1; we don't use device mode.)
