"""
pio_splash.py -- PlatformIO pre-build step: bake assets/splash.png into
lib/render/splash_image.c before every build (platformio.ini,
extra_scripts), so a new splash PNG can't be forgotten.

PlatformIO runs this in its own Python, which lacks Pillow, so the work
happens in the tools environment through `uv run splash.py`. splash.py
leaves the C file alone when nothing changed, so builds stay quick.

If uv isn't installed the build goes on with the committed
splash_image.c, with a warning. If uv is there but the conversion fails
(a wrong-size PNG, say), the build stops: carrying on would ship an image
that doesn't match the PNG.
"""

import shutil
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821 -- provided by PlatformIO's SCons

TOOLS = Path(env.subst("$PROJECT_DIR")) / "tools"  # noqa: F821

uv = shutil.which("uv")
if uv is None:
    print("pio_splash: uv not found; using the committed "
          "lib/render/splash_image.c")
else:
    result = subprocess.run([uv, "run", "--quiet", "splash.py"], cwd=TOOLS)
    if result.returncode != 0:
        sys.exit("pio_splash: tools/splash.py failed (see above)")
