#!/usr/bin/env bash
# Builds jp.zip, the Japanese overlay for the web root fs: IPA fonts (vn/overlay-jp) and a
# ja_JP.utf8 locale, which is the root fs's en_US.utf8 under the ja_JP name (Wine only needs
# the name to pick code page 932), plus the root fs's Wine prefix registry with unneeded
# services turned off (see below), and Wine 11.0's wineboot and explorer with vn/wine/*.patch
# (build-wine-boot.sh, needs Docker): they skip shell32/wininet/rpcss work a game doesn't use,
# and wineboot imports jp-fonts.reg (below) into prefixes older than this jp.zip's, and quartz hands
# MPEG-1 movies to the browser page (/dev/vnbox-movie, vnbox-movie.js), and dsound mixes 16-bit
# and float sound without a function call per sample; and ntdll.so and
# wineserver built with ntsync support (build-wine-ntsync.sh); and the WebGL wined3d.dll with
# vn/webgl/*.patch (build-webgl-wined3d.sh).
# Usage: vn/build-jp-overlay.sh <TinyCore15Wine11.0-web.zip> <out/jp.zip>
set -euo pipefail
root_zip=$(realpath "$1")
out=$(realpath -m "$2")
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
boot_services="Eventlog PlugPlay NDIS nsiproxy"
boot_devices="WINEBTH WINEBUS WINEUSB"

cp -r "$here/overlay-jp/." "$work/"
unzip -q "$root_zip" 'usr/lib/locale/en_US.utf8/*' -d "$work/en"
mkdir -p "$work/usr/lib/locale"
mv "$work/en/usr/lib/locale/en_US.utf8" "$work/usr/lib/locale/ja_JP.utf8"
rm -rf "$work/en"
test -s "$work/usr/lib/locale/ja_JP.utf8/LC_CTYPE"

# Font replacements (jp-fonts.reg, imported by the patched wineboot): the Windows Japanese font names
# as aliases of the IPA fonts. A version 5 .reg file is UTF-16LE, so the names don't depend on
# the code page reg import runs under.
cat > "$work/fonts.txt" <<'FONTS'
MS Gothic|IPAGothic
MS PGothic|IPAPGothic
MS UI Gothic|IPAPGothic
MS Mincho|IPAMincho
MS PMincho|IPAPMincho
ＭＳ ゴシック|IPAGothic
ＭＳ Ｐゴシック|IPAPGothic
ＭＳ 明朝|IPAMincho
ＭＳ Ｐ明朝|IPAPMincho
FONTS
{
  printf 'Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software\\Wine\\Fonts\\Replacements]\r\n'
  while IFS='|' read -r name font; do
    printf '"%s"="%s"\r\n' "$name" "$font"
  done < "$work/fonts.txt"
  # Faster Wine boot: services.exe starts every auto-start service and root device before the
  # game may run (~1.4 s in VNBox). A VN needs none of these: the event log, Plug and Play,
  # networking drivers, and the Bluetooth/HID/USB buses (no such devices in the browser; HID
  # means no gamepads). Kept: mountmgr (drives, discs). Wine only reads this at boot, so the
  # import takes effect from the next launch; the prefix below covers the first one.
  printf '\r\n'
  for s in $boot_services; do
    printf '[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\%s]\r\n"Start"=dword:00000004\r\n\r\n' "$s"
  done
  for d in $boot_devices; do
    printf '[-HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Enum\\ROOT\\WINE\\%s]\r\n\r\n' "$d"
  done
  # No desktop shortcuts or file associations: winemenubuilder -a -r is a process per launch for nothing.
  printf '[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\RunServices]\r\n"winemenubuilder"=-\r\n\r\n'
} > "$work/jp-fonts.utf8"
{ printf '\xff\xfe'; iconv -f UTF-8 -t UTF-16LE "$work/jp-fonts.utf8"; } > "$work/opt/wine/share/wine/jp-fonts.reg"
rm "$work/jp-fonts.utf8"

# Faster boot, part 2: patched wineboot (no shell32/wininet for its registry defaults) and explorer
# (no Program Manager DDE or ShellWindows on a desktop without the shell), with their LGPL source;
# quartz with browser movies, and dsound with the call-free mixer.
"$here/build-wine-boot.sh" "$work/opt/wine/lib/wine/i386-unix"
mkdir -p "$work/opt/wine/share/wine/vnbox-patches"
cp "$here"/wine/*.patch "$work/opt/wine/share/wine/vnbox-patches/"
printf 'wineboot.exe.so, explorer.exe.so, quartz.dll.so and dsound.dll.so in opt/wine/lib/wine/i386-unix are Wine 11.0\n(LGPL-2.1, https://dl.winehq.org/wine/source/11.0/wine-11.0.tar.xz) with these\npatches, built by vn/build-wine-boot.sh in https://github.com/blankiekun/VNBox.\n' \
  > "$work/opt/wine/share/wine/vnbox-patches/README"

# ntsync: the root fs's ntdll.so and wineserver rebuilt with linux/ntsync.h, so event, mutex and
# semaphore waits skip wineserver once the kernel has /dev/ntsync (falls back to the server without).
"$here/build-wine-ntsync.sh" "$work/ntsync"
mv "$work/ntsync/ntdll.so" "$work/opt/wine/lib/wine/i386-unix/"
mkdir -p "$work/opt/wine/bin"
mv "$work/ntsync/wineserver" "$work/opt/wine/bin/"
mv "$work/ntsync/patches" "$work/opt/wine/share/wine/vnbox-patches/rootfs"
rm -r "$work/ntsync"
printf '\nntdll.so (i386-unix) and wineserver (bin) are Wine 11.0 with the root fs patches in rootfs/\n(tools/buildWine/wine_builds.json), configured with Linux 6.14'"'"'s include/uapi/linux/ntsync.h,\nbuilt by vn/build-wine-ntsync.sh in https://github.com/blankiekun/VNBox.\n' \
  >> "$work/opt/wine/share/wine/vnbox-patches/README"

# WebGL WineD3D: Direct3D 10/11 SM4 shaders run on GLSL ES 3.00, and draws with no vertex shader
# bound are skipped instead of crashing (Muramasa EN).
"$here/build-webgl-wined3d.sh" "$work/home/username/.wine/drive_c/webgl"
mkdir -p "$work/opt/wine/share/wine/vnbox-patches/webgl"
cp "$here"/webgl/*.patch "$work/opt/wine/share/wine/vnbox-patches/webgl/"
printf '\nhome/username/.wine/drive_c/webgl/wined3d.dll is Wine 11.0 with the WebGL series v47\n(tools/wineTests/webgl-test-divergences-v47.json) and the patches in webgl/, built by\nvn/build-webgl-wined3d.sh in https://github.com/blankiekun/VNBox.\n' \
  >> "$work/opt/wine/share/wine/vnbox-patches/README"

# The root fs's prefix with the same changes, so a new prefix boots fast from the first launch and
# wineboot never imports into it: system.reg with the services off and no winemenubuilder, user.reg
# with the font replacements and the file's hash (HKCU\Software\VNBox JpFontsReg, FNV-1a as in
# wineboot-import-jp-fonts.patch). Existing prefixes keep their own .reg files (they win over the
# zips) and get the import once.
mkdir -p "$work/home/username/.wine"
unzip -p "$root_zip" home/username/.wine/system.reg | python3 -c '
import re, sys
services, devices = sys.argv[1].split(), sys.argv[2].split()
out, n, menu = [], 0, 0
for block in re.split(r"(?m)^(?=\[)", sys.stdin.read()):
    m = re.match(r"\[System\\\\ControlSet001\\\\(Services|Enum\\\\ROOT\\\\WINE)\\\\([^]\\\\]+)", block)
    if m and m.group(1) == "Services" and m.group(2) in services and "]" == block[m.end()]:
        block, k = re.subn(r"\"Start\"=dword:\w+", "\"Start\"=dword:00000004", block)
        n += k
    elif m and m.group(1) != "Services" and m.group(2) in devices:
        continue
    elif block.startswith("[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\RunServices]"):
        block, menu = re.subn(r"(?m)^\"winemenubuilder\"=.*\n", "", block)
    out.append(block)
assert n == len(services), "a service to disable is missing from system.reg"
assert menu == 1, "winemenubuilder is missing from system.reg RunServices"
sys.stdout.write("".join(out))
' "$boot_services" "$boot_devices" > "$work/home/username/.wine/system.reg"
unzip -p "$root_zip" home/username/.wine/user.reg | python3 -c '
import sys
fonts, reg = sys.argv[1:]
data = open(reg, "rb").read()
h = 2166136261
for b in data:
    h = ((h ^ b) * 16777619) & 0xFFFFFFFF
# Wine registry strings: \\ and \" escaped, other non-ASCII as UTF-16 units (\x + 4 digits, as wineserver saves them)
def esc(s):
    s = s.replace("\\", "\\\\").replace("\"", "\\\"")
    u = s.encode("utf-16-le")
    units = (int.from_bytes(u[i:i + 2], "little") for i in range(0, len(u), 2))
    return "".join(chr(c) if c < 128 else "\\x%04x" % c for c in units)
text = sys.stdin.read()
assert "[Software\\\\Wine\\\\Fonts\\\\Replacements]" not in text and "[Software\\\\VNBox]" not in text
pairs = [l.rstrip("\n").split("|") for l in open(fonts, encoding="utf-8") if l.strip()]
text += "\n[Software\\\\Wine\\\\Fonts\\\\Replacements]\n" + "".join("\"%s\"=\"%s\"\n" % (esc(n), esc(f)) for n, f in pairs)
text += "\n[Software\\\\VNBox]\n\"JpFontsReg\"=\"fnv1a-%08x\"\n" % h
sys.stdout.write(text)
' "$work/fonts.txt" "$work/opt/wine/share/wine/jp-fonts.reg" > "$work/home/username/.wine/user.reg"
rm "$work/fonts.txt"
rm -f "$out"
(cd "$work" && find usr opt home | LC_ALL=C sort | zip -q -X -9 "$out" -@)
