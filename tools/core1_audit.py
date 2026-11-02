#!/usr/bin/env python3
"""Check that nothing core 1 can run lives in flash.

    tools/core1_audit.py .pio/build/pizero/firmware.elf

Core 1 must stay out of XIP flash, because flash disappears while the
BOOTSEL button is read and while settings are written. This walks the
call graph (bl/b targets in objdump output) from core 1's entry points
and reports any reachable function outside SRAM. Linker veneers count as
flash, since they exist to jump there. Exit status 1 if anything is found.
"""
import re
import subprocess
import sys
from pathlib import Path

OBJDUMP = Path.home() / ".platformio/packages/toolchain-rp2040-earlephilhower/bin/arm-none-eabi-objdump"
ROOTS = ["core1_scanout", "dvi_dma0_irq", "dvi_dma1_irq"]

# Flash calls that are acceptable, and why. Anything else fails the audit.
ALLOWED = {
    # Run once by core 1 before it enters the scanout loop; video.c only
    # reports video_flash_safe() after that point.
    "__dvi_register_irqs_this_core_veneer": "core 1 start-up, before the loop",
    "__dvi_start_veneer": "core 1 start-up, before the loop",
    # Only reached when an invariant is already broken.
    "__panic_veneer": "fatal path",
    "____assert_func_veneer": "fatal path",
}
SRAM_START = 0x20000000


def main() -> int:
    text = subprocess.run([str(OBJDUMP), "-d", "--no-show-raw-insn", sys.argv[1]],
                          capture_output=True, text=True, check=True).stdout
    funcs: dict[str, tuple[int, set[str]]] = {}
    current = None
    for line in text.splitlines():
        head = re.match(r"^([0-9a-f]{8}) <([^>]+)>:", line)
        if head:
            current = head.group(2)
            funcs[current] = (int(head.group(1), 16), set())
            continue
        call = re.search(r"\sb(?:l|lx)?(?:\.[nw])?\s+[0-9a-f]+ <([^>+]+)", line)
        if current and call and call.group(1) != current:
            funcs[current][1].add(call.group(1))

    seen, stack, bad = set(), list(ROOTS), []
    while stack:
        name = stack.pop()
        if name in seen or name not in funcs:
            continue
        seen.add(name)
        addr, calls = funcs[name]
        if (addr < SRAM_START or name.endswith("_veneer")) and name not in ALLOWED:
            bad.append(name)
        stack.extend(calls)
    print(f"core 1 reaches {len(seen)} functions; in flash: {sorted(bad) or 'none'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
