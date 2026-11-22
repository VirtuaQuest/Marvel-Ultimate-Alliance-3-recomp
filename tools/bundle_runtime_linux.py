#!/usr/bin/env python3
"""Copy the runtime's media dependencies; keep host graphics drivers and libc."""
import pathlib
import re
import shutil
import subprocess

root = pathlib.Path(__file__).resolve().parent.parent
destination = root / "build" / "lib"
destination.mkdir(parents=True, exist_ok=True)
host = re.compile(r"^(?:lib(?:c|m|dl|pthread|rt|stdc\+\+|gcc_s)\.so|ld-linux|lib(?:vulkan|SDL3|drm|gbm|GL|EGL|va|vdpau|X11|xcb|wayland))")
listing = subprocess.check_output(["ldd", str(root / "build" / "mua3recomp")], text=True)
if "not found" in listing:
    raise SystemExit("Run bundling inside the development environment with all libraries installed.")
count = 0
for line in listing.splitlines():
    match = re.match(r"\s*(\S+) => (/\S+) ", line)
    if match and not host.match(match[1]):
        shutil.copy2(pathlib.Path(match[2]).resolve(), destination / match[1])
        count += 1
print(f"Bundled {count} runtime libraries in {destination}")
