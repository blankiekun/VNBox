# VNBox tuning options

VNBox options are URL params of `boxedwine.html` (for example `boxedwine.html?...&fps=30&nativeLibc=0`).
`vnbox-options.json`, shipped next to the engine in every `vnbox-web` release, lists them so a wrapper can build a
tuning menu without hard-coding names.

## Using it from a wrapper

1. Fetch `vnbox-options.json` from the engine's folder (the same folder as `boxedwine.js`).
2. Check `schema` (now `1`); skip the menu if you don't know the version.
3. Build one control per entry in `options`, grouped by `group` (in the order of `groups`):
   - `bool`: a switch. Send `urlValues.true` or `urlValues.false` (they differ per option, e.g. `fitWindow=true`
     but `nativeLibc=0`).
   - `int` / `float`: a number field or slider from `min` to `max`.
   - `choice`: a select over `choices` (`value`, `label`, optionally `experimental`).
   - Show `label`, with `description` as help text. Hide or mark `experimental` entries.
4. Add the chosen values to the player URL as `name=value`. Leave a param out to get its `default`: send only what
   the user changed.
5. Options with `restart: true` (all of them today) are read once at startup: reload the player to apply a change.

## Adding an option

Read it in C++ with `vnboxUrlInt` / `vnboxUrlIs` (`include/vnboxparam.h`) or in JS with `vnboxParam` (`lazyfs.js`),
always with the name as a string literal, and add its entry to `project/emscripten/vnbox-options.json`.
`node project/emscripten/vnbox-options.check.mjs` (run by CI) fails when code reads an option the file does not list,
when the file lists one nothing reads, or when an entry is malformed.
