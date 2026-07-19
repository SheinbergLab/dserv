package main

import (
	"os"
	"testing"
)

// The /extio/setup endpoint serves embedded copies of wiznet-io/provision.sh
// and pt.json. They are SEPARATE committed files (go:embed can't reach a
// sibling package dir), so a plain `go build .` that skips `make sync-provision`
// can commit and ship a copy that has drifted from the canonical source -- which
// is exactly how a stale provision.sh once shipped ignoring PT_JSON.
//
// This test fails on drift. Fix: `make sync-provision` (or `make build`) and
// re-commit the copies. It skips when the canonical sources aren't present
// (e.g. a partial checkout) rather than failing spuriously.
func TestEmbeddedProvisionInSync(t *testing.T) {
	cases := []struct {
		name     string
		embedded string
		src      string
	}{
		{"provision.sh", provisionScript, "../wiznet-io/provision.sh"},
		{"pt.json", partitionSpec, "../wiznet-io/pt.json"},
		{"pt-pico2w.json", partitionSpecPico2w, "../wiznet-io/pt-pico2w.json"},
	}
	for _, c := range cases {
		src, err := os.ReadFile(c.src)
		if err != nil {
			t.Skipf("canonical %s not present (%v) -- skipping drift check", c.src, err)
			continue
		}
		if string(src) != c.embedded {
			t.Errorf("embedded %s has drifted from %s -- run `make sync-provision` and re-commit the copy", c.name, c.src)
		}
	}
}
