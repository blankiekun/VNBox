# End-to-end checks

Six checks of one VNBox build through the real site UI, in Firefox and Chromium, off-screen (Xvfb), with the sound on a null sink of their own. Correctness only: nothing here measures speed.

## Running

```
E2E_PLAYWRIGHT=DIR project/emscripten/e2e/run.sh --site SITE_COPY --game GAME_DIR --work WORK_DIR
```

- `E2E_PLAYWRIGHT`: a dir whose `node_modules` has `playwright`, with both browsers installed.
- `--site`: a copy of the site (`index.html`, `js/`, `vnbox/`). It is copied before the run and only read. Its engine is the build tested, unless `--engine DIR` or `--tag TAG` says another.
- `--game`: the folder of 信天翁航海録. The checks know this game's screens and no other's. Read-only.
- `--work`: profiles and logs. A profile holds the game's copy in OPFS, so it is the size of the game; keep it between runs.

The first run in a browser copies the game into its profile. Needs `xvfb-run`, `pactl`, `parec`, and for `--tag`, `gh`. All options are at the top of `run.sh`. Keep a machine's own paths in a local wrapper, not here.

Each check prints one line, `PASS`, `FAIL` or `INFO`, then each browser prints an `overall` line. Exit: 0 all counted checks passed, 1 a check failed, 2 the run couldn't start. Logs per check are in `WORK_DIR/logs/<stamp>/<browser>/<check>/`.

## Seeing a check fail

`--break NAME` makes one input bad on purpose, on the run's copy of the site. Use it with `--only`, since other checks may fail on it too:

```
run.sh ... --browsers firefox --only movie --break no-movie-player
```

The names and what each breaks are in `BREAKS` in `suite.mjs`. A profile keeps its copy of a root or overlay zip under the sum in `vnbox-zips.sha256` and never reads the served bytes again, so damaging a served zip alone changes nothing; `corrupt-jp-zip` changes the sum too.

## What the saves check doesn't cover

It can see a lost save: with `--break close-on-write` the frame goes in the same tick the save's write ends, and the check fails with the save not in the store. Without that break it can't reach the race: by the time the game shows the slot filled, the store has been written, even if the frame is then removed at once. So a pass says the site's own close path keeps a save at normal speed. It says nothing about a persist that is slow or has failed and is waiting to retry; the close race itself is tested by `lazyfs.check.mjs` as changed in #127.
