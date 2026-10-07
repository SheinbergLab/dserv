# viewtest — DG Viewer against real data, without an install

Serves the checkout's `www/dg_viewer.html` statically, with a viewer
plugin under test dropped in beside it and real `.trials.dgz` files to
load, so a plugin change can be checked before it reaches
`/usr/local/dserv/www/viewers/` (by `sudo cp` here, by `ess_sync`'s
`_install_viewers` on a rig).

- `dg_viewer.html`, `js/`, `fonts/` are symlinks into `../../www`, so the
  page is always the checkout's current one.
- `viewers/<system>.js` is the plugin copy the page imports (keyed by the
  dgz's `system` column). Not tracked; put there by `stage`.
- `data/` holds symlinks to trials files. Not tracked.

```sh
tools/viewtest/stage ~/systems/ess/planko/planko_viewer.js \
    ~/data/trials/some_session.trials.dgz
```

Then start `dgviewer-test` from `.claude/launch.json` (it runs
`tools/devserver.py`, which disables caching, so a plain reload picks up
a re-staged plugin) and open the URL `stage` prints:
`http://localhost:8137/dg_viewer.html?file=data/<name>.dgz`.

Not to be confused with `tools/viztest`, which renders a protocol's
experimenter viz (`set_viz_config`) offline.
