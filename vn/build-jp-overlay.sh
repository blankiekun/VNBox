#!/usr/bin/env bash
# Builds jp.zip, the Japanese overlay for the web root fs: IPA fonts (vn/overlay-jp) and a
# ja_JP.utf8 locale, which is the root fs's en_US.utf8 under the ja_JP name (Wine only needs
# the name to pick code page 932).
# Usage: vn/build-jp-overlay.sh <TinyCore15Wine11.0-web.zip> <out/jp.zip>
set -euo pipefail
root_zip=$(realpath "$1")
out=$(realpath -m "$2")
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

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
} > "$work/jp-fonts.utf8"
{ printf '\xff\xfe'; iconv -f UTF-8 -t UTF-16LE "$work/jp-fonts.utf8"; } > "$work/opt/wine/share/wine/jp-fonts.reg"
rm "$work/jp-fonts.utf8"
rm -f "$out"
(cd "$work" && find usr opt | LC_ALL=C sort | zip -q -X -9 "$out" -@)
