# www/js/vendor — locally served third-party JS

Everything here exists so the web GUIs work with **no internet access**
(air-gapped rigs). Nothing in `www/` may import from a CDN at runtime;
vendor it here instead.

## codemirror.js

Single ESM bundle consumed by `www/js/TclEditor.js` (the script editor in
ess_workbench.html, dlsh.html, command_reference.html, DGTableViewer.html).
It re-exports exactly the symbols TclEditor destructures. Bundled versions
are listed in the banner comment at the top of the file.

Regenerate (needs node + network, any machine):

```sh
mkdir cmbundle && cd cmbundle
npm init -y
npm install @codemirror/state@6 @codemirror/view@6 @codemirror/commands@6 \
    @codemirror/language@6 @codemirror/autocomplete@6 @codemirror/search@6 \
    @codemirror/legacy-modes@6 @uiw/codemirror-theme-dracula@4 \
    @babel/runtime esbuild
# entry.js: the "export { … } from '@codemirror/…'" list — copy it from the
# banner symbols or from the import block in www/js/TclEditor.js.
npx esbuild entry.js --bundle --format=esm --minify --target=es2020 \
    --outfile=codemirror.js --legal-comments=none
```

Then update the banner versions and drop the file here. Bundle in ONE
esbuild pass — loading @codemirror packages as separate per-package
bundles duplicates @codemirror/state and breaks CodeMirror (instanceof
checks fail across instances).

## ace/ (tutorial.html)

Ace editor files fetched verbatim from cdnjs (version in each file's
header). tutorial.html loads `ace.js` + `ext-language_tools.js`; Ace then
lazy-loads mode/theme files from the same directory, so any
`setMode`/`setTheme` value used by tutorial.html needs its `mode-*.js` /
`theme-*.js` file present here too.
