# VeeTee 2350 Style Guide

Read this before touching the code. It applies to humans and to coding
assistants (Claude, Copilot, etc.). If you're an assistant: follow it, and
update it when the project settles on a new convention. Don't invent a
parallel one.

## 1. Language and toolchain

- **C23** (`-std=gnu23`) on GCC 14, which arduino-pico ships. Use C23
  features where they make things clearer: `nullptr`, `bool` as a keyword,
  `constexpr` objects, `static_assert` without a message, `[[nodiscard]]`,
  `[[maybe_unused]]`, digit separators (`393'504`), binary literals,
  `typeof`.
- **No C++ in our code.** The only C++ file is `src/main.cpp`, a shim that
  hands Arduino's `setup()`/`loop()` to C entry points. Everything else is
  `.c`/`.h`. C++ libraries we depend on are wrapped behind a C API.
- Firmware environment: PlatformIO, `maxgerhardt/platform-raspberrypi`,
  arduino-pico core, board `waveshare_rp2350_pizero`.
- Host environment: `pio test -e native` builds hardware-independent
  modules (`term/`, `render/` maths, `input/`) for the Mac and runs Unity
  tests there.
- Call pico-sdk APIs (`hardware/*.h`, `pico/*.h`) directly. Only use
  Arduino APIs where the core forces us to (USB bring-up, `setup`/`loop`).

## 2. Formatting: BSD Kernel Normal Form (style(9)), with one exception

- Indent with **tabs** (8 wide). Continuation lines get 4 spaces past the
  indent.
- Wrap lines at 80 columns, but don't mangle a string or a table just to
  hit that.
- A function's return type goes on its own line, with the name at column 0:

  ```c
  static int
  screen_scroll_up(struct screen *scr, int top, int bottom, int count)
  {
  	...
  }
  ```

- Opening brace on the same line for control statements and struct
  definitions. On its own line for function bodies.
- Space after keywords (`if (`, `for (`, `while (`, `switch (`, `return`).
  No space after a function name.
- `switch`: `case` labels are not indented relative to `switch`.
  Fallthrough is marked `[[fallthrough]];`.
- Braces are optional for single statements, unless any branch of the same
  `if`/`else` chain uses them.
- Pointers bind to the name: `char *buf`, not `char* buf`.
- `return` values are not parenthesised: `return count;`.

**The exception:** use descriptive variable names. Write `row`, `col`,
`glyph_index`, `cell_count`, not `r`, `c`, `gi`, `n`. Loop counters can be
`i`/`j` only when they are nothing but counters.

## 3. Naming

| Thing                 | Convention                     | Example                    |
|-----------------------|--------------------------------|----------------------------|
| Functions, variables  | `snake_case`, module prefix    | `video_init`, `term_feed`  |
| Types                 | `struct name`, no typedefs for structs | `struct cell`      |
| Opaque handles        | `typedef struct x x_t` is OK only for opaque types in public headers | |
| Enums                 | `enum name { NAME_VALUE }`     | `CELL_ATTR_BOLD`           |
| Macros / constants    | `UPPER_SNAKE`                  | `TERM_COLS`                |
| File-local            | `static`, no prefix needed     | `static void emit_row()`   |

Every public symbol starts with its module's prefix (`video_`, `usbio_`,
`flashcfg_`, `board_`, `term_`, `render_`, `grid_`, `font_`, `keys_`,
`settings_`, `setup_`).

## 4. Project layout and layering

```
src/            main.cpp shim and app glue (app.c: wires modules together)
include/        public headers that cross module boundaries
lib/hal/        hardware abstraction: video (DVI), usbio (CDC), clocks, flash
lib/term/       escape-sequence handling (libvterm state) -> cell grid model
lib/render/     cell grid + font -> scanlines (pure, host-testable)
lib/font/       font formats (PSF2) and the embedded default font
lib/input/      keyboard byte stream -> key events (pure, host-testable)
lib/setup/      settings record + SET-UP screen (pure, host-testable)
lib/PicoDVI/    vendored libdvi (BSD-3), see its PATCHES.md
lib/libvterm/   vendored libvterm (MIT)
test/           Unity tests, run on the host (native env)
tools/          host scripts (font conversion, capture, etc.)
docs/           design notes, spike reports
```

Dependencies only point **downward**: `src` → `term`/`render`/`input` →
`font` → `hal`. Pure modules (`term`, `render`, `input`, `font`) never
include a `hardware/*` or `pico/*` header, so they always compile on the
host. Anything that touches silicon lives in `lib/hal/`.

## 5. Comments and documentation

The rule is **"show your work"**:

- Every file opens with a block comment saying what the module does and
  where it sits in the layering.
- Every non-trivial function gets a comment explaining *why* it works the
  way it does: invariants, units, ownership, which core calls it, and its
  timing budget.
- Magic numbers either get a named constant or a comment that derives the
  number (e.g. "25.2 MHz pixel clock × 10 bits/symbol = 252 MHz sysclk").
- Cite the spec for every escape sequence handled: DEC STD 070, the
  VT100/VT220 manuals, or xterm's ctlseqs. Format: `/* DECSTBM, ctlseqs
  "CSI Ps ; Ps r" */`.
- Long design discussions go in `docs/` and the code links to them.

## 6. Embedded rules

- **No dynamic allocation after init.** `malloc` is only allowed during
  startup, and only through a module's `*_init`. Prefer static storage.
  Memory budgets live in `docs/memory.md`.
- **Core ownership is explicit.** Core 1 runs DVI scanout and TMDS
  encoding only. Core 0 runs USB, parsing and screen updates. Anything
  shared between the cores is documented at its declaration, with the
  synchronisation used (spinlock, queue, or single-writer + `volatile` +
  memory barrier).
- Code that runs in the scanline path is marked `__not_in_flash_func` or
  placed in RAM, and its per-line cycle budget is noted in a comment.
  Small helpers it calls must be `HOT_INLINE` (always_inline): the core
  builds at -Os, which out-lines `static inline` functions into flash.
- **Core 1 must never execute from flash.** BOOTSEL reads and settings
  writes take XIP away. `python3 tools/core1_audit.py
  .pio/build/pizero/firmware.elf` checks this. Run it after touching
  anything core 1 can reach.
- Use fixed-width types (`uint8_t`, `uint32_t`) for hardware and
  wire-format data, and `int`/`size_t` for counts and indices.
- `static_assert` every layout assumption (struct sizes, table lengths).

## 7. Errors

- Functions that can fail return `bool` (true = success) or a negative
  errno-style `int`. Mark them `[[nodiscard]]`.
- Hardware init failures during boot go to `panic()` with a message. A
  terminal that can't show a picture has nothing better to do.
- Malformed input from the host is **never** fatal. Drop it and keep
  going. Terminals must be robust against garbage on the line.

## 8. Commits

Standard Commit format: `<type>: <summary>`, a blank line, then the body.
Use single-word types (`feature`, `fix`, `docs`, `build`, `refactor`,
`test`, `hal`, `term`, `render`). End with the `Co-Authored-By` trailer
when an assistant wrote the change.

## 9. Third-party code

Vendored code lives under `lib/<name>/`, keeps its own style and license
file, and gets a `PATCHES.md` that lists every local modification and the
reason for it. Don't reformat vendored code.
