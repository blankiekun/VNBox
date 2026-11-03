#!/usr/bin/env python3
"""Checks a finished jp.zip (build-jp-overlay.sh runs this last, so a broken zip fails the build, not a release).
Catches: a binary missing or built for the wrong architecture, a patch dropped from a build (its export or string gone,
or its file missing from the zip's source notes), and a user.reg whose JpFontsReg hash is not jp-fonts.reg's, which
would import the fonts again on every prefix or never.
Usage: vn/check-jp-zip.py <jp.zip>"""
import glob, os, re, subprocess, sys, tempfile, zipfile

here = os.path.dirname(os.path.abspath(__file__))
unix = "opt/wine/lib/wine/i386-unix/"
patches = "opt/wine/share/wine/vnbox-patches/"
wined3d = "home/username/.wine/drive_c/webgl/wined3d.dll"
# Each 32-bit ELF jp.zip replaces, and a string only its patched build has (b"" = the patch leaves none to look for).
binaries = {
    unix + "ntdll.so": b"",
    "opt/wine/bin/wineserver": b"/dev/ntsync",
    unix + "win32u.so": b"WINE_VNBOX_SPI_CACHE",
    unix + "winex11.so": b"",
    unix + "wineboot.exe.so": "jp-fonts.reg".encode("utf-16-le"),
    unix + "explorer.exe.so": b"",
    unix + "quartz.dll.so": "vnbox-movie".encode("utf-16-le"),
    unix + "dsound.dll.so": b"VNBox: buffer",
}
win32u_exports = {"vnbox_glyphm", "vnbox_glyphm_ft", "vnbox_dibdrv_funcs"}
others = [wined3d, "opt/wine/share/wine/jp-fonts.reg", "home/username/.wine/system.reg", "home/username/.wine/user.reg",
          "usr/lib/locale/ja_JP.utf8/LC_CTYPE", patches + "README", patches + "rootfs/"]
others += [patches + os.path.basename(p) for p in glob.glob(f"{here}/wine/*.patch")]
others += [patches + "webgl/" + os.path.basename(p) for p in glob.glob(f"{here}/webgl/*.patch")]

errors = []
z = zipfile.ZipFile(sys.argv[1])
names = set(z.namelist())
errors += [f"missing: {n}" for n in others if n not in names]
for name, needle in binaries.items():
    if name not in names:
        errors.append(f"missing: {name}")
        continue
    data = z.read(name)
    # e_ident: "\x7fELF", class 1 = 32-bit; e_machine at 18: 3 = i386
    if data[:5] != b"\x7fELF\x01" or data[18:20] != b"\x03\x00":
        errors.append(f"not a 32-bit i386 ELF: {name}")
    if needle not in data:
        errors.append(f"built without its patch ({needle.decode()} not found): {name}")
if wined3d in names:
    data = z.read(wined3d)
    pe = int.from_bytes(data[0x3c:0x40], "little")
    if data[:2] != b"MZ" or data[pe:pe + 6] != b"PE\0\0\x4c\x01":
        errors.append(f"not a 32-bit PE: {wined3d}")

if unix + "win32u.so" in names:
    with tempfile.NamedTemporaryFile() as f:
        f.write(z.read(unix + "win32u.so"))
        f.flush()
        syms = subprocess.run(["readelf", "--dyn-syms", "-W", f.name], check=True, capture_output=True, text=True).stdout
    # a defined symbol: the Ndx column is a section number, not UND
    defined = {m[1] for m in re.finditer(r"(?m)^\s*\d+:\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+\d+\s+(\S+)$", syms)}
    errors += [f"win32u.so does not export {e}" for e in sorted(win32u_exports - defined)]

if {"opt/wine/share/wine/jp-fonts.reg", "home/username/.wine/user.reg"} <= names:
    h = 2166136261  # FNV-1a, as wineboot-import-jp-fonts.patch
    for b in z.read("opt/wine/share/wine/jp-fonts.reg"):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    if f'"JpFontsReg"="fnv1a-{h:08x}"' not in z.read("home/username/.wine/user.reg").decode("utf-8", "replace"):
        errors.append(f"user.reg's JpFontsReg is not jp-fonts.reg's hash fnv1a-{h:08x}")

if errors:
    sys.exit("jp.zip check failed:\n  " + "\n  ".join(errors))
print(f"jp.zip check ok: {len(binaries)} binaries, {len(others)} other files")
