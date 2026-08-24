package main

import "testing"

// The registry names a script by (protocol, type) and REBUILDS the filename
// from that pair, so a mis-derived type does not error — it writes over a
// different script. deriveSystemScriptType had no _viewer case and fell
// through to "system", which made `push --add` on a system-level viewer
// (planko_viewer.js) a PUT to (protocol "", type "system") — i.e. over
// planko.tcl, with JavaScript.
//
// Reference implementation is ess_scripts-1.0.tm's _derive_proto_type; these
// cases are that proc's branches.
func TestDeriveScriptTypes(t *testing.T) {
	sys := []struct{ file, want string }{
		{"planko.tcl", "system"},
		{"planko_extract.tcl", "extract"},
		{"planko_analyze.tcl", "analyze"},
		{"planko_viewer.js", "viewer"},
	}
	for _, c := range sys {
		if got := deriveSystemScriptType(c.file); got != c.want {
			t.Errorf("deriveSystemScriptType(%q) = %q, want %q", c.file, got, c.want)
		}
	}

	proto := []struct{ protocol, file, want string }{
		{"bounce", "bounce.tcl", "protocol"},
		{"bounce", "bounce_loaders.tcl", "loaders"},
		{"bounce", "bounce_stim.tcl", "stim"},
		{"bounce", "bounce_variants.tcl", "variants"},
		{"bounce", "bounce_viewer.js", "viewer"},
		// A viewer is a viewer whatever it is named — the Tcl checks the
		// _viewer suffix before stripping the protocol prefix, and the
		// prefix strip alone would return "planko_viewer" here.
		{"bounce", "planko_viewer.js", "viewer"},
	}
	for _, c := range proto {
		if got := deriveScriptType(c.protocol, c.file); got != c.want {
			t.Errorf("deriveScriptType(%q, %q) = %q, want %q", c.protocol, c.file, got, c.want)
		}
	}
}
