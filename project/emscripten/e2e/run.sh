#!/bin/bash
# End-to-end checks of one VNBox build on the real site UI, one PASS/FAIL/INFO line per check, in Firefox and Chromium,
# off-screen (Xvfb) with the sound on a null sink of its own.
#
#   e2e/run.sh --site DIR --game DIR --work DIR [--engine DIR | --tag TAG] [--browsers firefox,chromium] [--only a,b] [--break NAME]
#
#   --site     a copy of the site (index.html, js/, vnbox/); its engine is the build tested unless one of the next two says another.
#              It is copied before the run and only read.
#   --engine   an engine dir (boxedwine.wasm, ...) to test instead, in the site's vnbox/engine.
#   --tag      a VNBox release to test instead, put on the copy by the site's own tools/update-vnbox.sh (needs gh).
#   --game     the game's folder (read-only). The checks know 信天翁航海録's screens, not any other game's.
#   --work     where profiles and logs go. A profile holds the game's copy in OPFS (the size of the game), so it's kept and reused.
#   --only     check names, comma separated.
#   --break    make a check's input bad on purpose, to see it fail: see BREAKS in suite.mjs.
#
# Env: E2E_PLAYWRIGHT   a dir whose node_modules has playwright (required).
#      E2E_SERVE        where the site's copy goes (default WORK/serve).
#      E2E_LOCK_DIR     a dir with the machine's shared run locks: when set, each browser's run takes a check slot there (.run.gate, .run.lock
#                       shared, .check.N.lock), so timing runs stay alone. E2E_CHECK_SLOTS: how many (default 2).
# Exit: 0 all counted checks passed, 1 a check failed, 2 the run couldn't start.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SITE= GAME= WORK= ENGINE= TAG= BROWSERS=firefox,chromium ONLY= BREAK=
while [ $# -gt 0 ]; do case $1 in
  --site) SITE=$2;; --game) GAME=$2;; --work) WORK=$2;; --engine) ENGINE=$2;; --tag) TAG=$2;;
  --browsers) BROWSERS=$2;; --only) ONLY=$2;; --break) BREAK=$2;;
  *) echo "unknown argument $1 (see the top of $0)"; exit 2;; esac; shift 2; done
[ -n "$SITE" ] && [ -n "$GAME" ] && [ -n "$WORK" ] && [ -n "${E2E_PLAYWRIGHT:-}" ] || { sed -n '2,20p' "$0"; exit 2; }
[ -f "$SITE/index.html" ] && [ -d "$GAME" ] || { echo "no site at $SITE or no game at $GAME"; exit 2; }
command -v xvfb-run >/dev/null || { echo "xvfb-run is needed: the browsers must not open a visible window"; exit 2; }

mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SERVE=${E2E_SERVE:-$WORK/serve}
# The site's copy, so a site updated during the run can't mix two builds, and a --break can't touch the original.
rm -rf "$SERVE"; mkdir -p "$SERVE"; cp -a "$SITE/." "$SERVE/" || exit 2
[ -n "$ENGINE" ] && { rm -rf "$SERVE/vnbox/engine"; cp -a "$ENGINE" "$SERVE/vnbox/engine" || exit 2; }
[ -n "$TAG" ] && { "$SERVE/tools/update-vnbox.sh" "$TAG" || exit 2; }

# Off-screen for sure: without these a browser can reach the desktop's Wayland display despite Xvfb.
unset WAYLAND_DISPLAY; export GDK_BACKEND=x11 MOZ_ENABLE_WAYLAND=0 E2E_OFFSCREEN=1

STATUS=0 STAMP=$(date +%Y%m%d-%H%M%S)
IFS=, read -ra LIST <<<"$BROWSERS"
for B in "${LIST[@]}"; do
  (
    if [ -n "${E2E_LOCK_DIR:-}" ]; then
      L=$E2E_LOCK_DIR
      exec 7>"$L/.run.gate"; flock -s 7; exec 9>"$L/.run.lock"; flock -s 9; exec 7>&-  # a waiting timing run holds the gate, so check runs queue behind it
      SLOT=; while [ -z "$SLOT" ]; do for i in $(seq 1 "${E2E_CHECK_SLOTS:-2}"); do exec 8>"$L/.check.$i.lock"; if flock -n 8; then SLOT=$i; break; fi; exec 8>&-; done
        [ -z "$SLOT" ] && sleep 5; done
    fi
    exec 6>"$WORK/.$B.lock"; flock 6  # one profile, one origin: a second run of this browser waits
    timeout 3600 xvfb-run -a -s "-screen 0 1600x1200x24" node "$HERE/suite.mjs" "$B" "$SERVE" "$GAME" "$WORK" "$WORK/logs/$STAMP/$B" "$ONLY" "$BREAK" 6>&- 7>&- 8>&- 9>&-
  ) || STATUS=$?
done
exit $STATUS
