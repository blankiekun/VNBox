#!/usr/bin/env bash
# Builds jp.zip, the Japanese overlay for the web root fs: IPA fonts (vn/overlay-jp) and a
# ja_JP.utf8 locale, which is the root fs's en_US.utf8 under the ja_JP name (Wine only needs
# the name to pick code page 932), plus the root fs's Wine prefix registry with unneeded
# services turned off (see below).
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

# Font replacements for boxedwine-shell.js's registry=jpfonts: the Windows Japanese font names
# as aliases of the IPA fonts. A version 5 .reg file is UTF-16LE, so the names don't depend on
# the code page reg import runs under.
{
  printf 'Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software\\Wine\\Fonts\\Replacements]\r\n'
  while IFS='|' read -r name font; do
    printf '"%s"="%s"\r\n' "$name" "$font"
  done <<'FONTS'
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
} > "$work/jp-fonts.utf8"
{ printf '\xff\xfe'; iconv -f UTF-8 -t UTF-16LE "$work/jp-fonts.utf8"; } > "$work/opt/wine/share/wine/jp-fonts.reg"
rm "$work/jp-fonts.utf8"

# The root fs's prefix with the same changes, so a new prefix boots fast from the first launch.
# Existing prefixes keep their own system.reg (it wins over the zips) and get the import.
mkdir -p "$work/home/username/.wine"
unzip -p "$root_zip" home/username/.wine/system.reg | python3 -c '
import re, sys
services, devices = sys.argv[1].split(), sys.argv[2].split()
out, n = [], 0
for block in re.split(r"(?m)^(?=\[)", sys.stdin.read()):
    m = re.match(r"\[System\\\\ControlSet001\\\\(Services|Enum\\\\ROOT\\\\WINE)\\\\([^]\\\\]+)", block)
    if m and m.group(1) == "Services" and m.group(2) in services and "]" == block[m.end()]:
        block, k = re.subn(r"\"Start\"=dword:\w+", "\"Start\"=dword:00000004", block)
        n += k
    elif m and m.group(1) != "Services" and m.group(2) in devices:
        continue
    out.append(block)
assert n == len(services), "a service to disable is missing from system.reg"
sys.stdout.write("".join(out))
' "$boot_services" "$boot_devices" > "$work/home/username/.wine/system.reg"
rm -f "$out"
(cd "$work" && find usr opt home | LC_ALL=C sort | zip -q -X -9 "$out" -@)
