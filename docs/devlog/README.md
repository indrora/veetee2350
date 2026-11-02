# VeeTee 2350 devlog

A running log of the build, one entry per micro-milestone, each with a
capture from the HDMI grabber (Genki ShadowCast 2) and notes on what
was learned.

1. [Choosing a parser: the libghostty-vt spike](001-choosing-a-parser.md)
2. [First light: DVI on GPIO 32–39](002-first-light.md)
3. [Fonts, and the case of the out-lined helper](003-fonts-and-text.md)
4. [It's a terminal](004-its-a-terminal.md)
5. [Two cores, one picture](005-two-cores.md)
6. [Switching fonts live](006-font-switching.md)
7. [A keyboard port, and making core 1 flash-free](007-keyboard-port.md)
8. [SET-UP](008-set-up.md)
9. [Six resolutions, two lurking bugs](009-resolutions.md)
10. [Knowing its own size](010-terminal-size.md)
11. [Emulation levels](011-emulation-level.md)
12. [Taking the tests](012-conformance.md)

How entries are made: capture with
`ffmpeg -f avfoundation -framerate 30 -i "ShadowCast 2" -frames:v 1 x.jpg`
from a terminal that has camera permission, then downscale into `img/`
with `sips -s format jpeg -s formatOptions 50 --resampleWidth 1280`.
