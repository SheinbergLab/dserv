package main

import (
	"os"
	"strings"
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

// Same guard for the PTP tooling served at /ptp/setup: the embedded ptp/
// copies are Makefile-mirrored from ../scripts and ../systemd.
func TestEmbeddedPTPInSync(t *testing.T) {
	canonical := map[string]string{
		"dserv-ptp-setup":               "../scripts/dserv-ptp-setup",
		"dserv-ptp-select-phc":          "../scripts/dserv-ptp-select-phc",
		"dserv-ptp4l@.service":          "../systemd/dserv-ptp4l@.service",
		"dserv-phc2sys@.service":        "../systemd/dserv-phc2sys@.service",
		"dserv-ptp4l-client@.service":   "../systemd/dserv-ptp4l-client@.service",
		"dserv-phc2sys-client@.service": "../systemd/dserv-phc2sys-client@.service",
		"chrony-grandmaster.conf":       "../systemd/chrony-grandmaster.conf",
	}
	if len(canonical) != len(ptpInstallFiles) {
		t.Errorf("test covers %d files but ptpInstallFiles has %d -- keep them in step", len(canonical), len(ptpInstallFiles))
	}
	for _, f := range ptpInstallFiles {
		embedded, err := ptpFS.ReadFile("ptp/" + f.embedName)
		if err != nil {
			t.Errorf("ptp/%s not embedded (%v) -- run `make sync-provision` and commit ptp/", f.embedName, err)
			continue
		}
		src := canonical[f.embedName]
		if src == "" {
			t.Errorf("no canonical source mapped for %s", f.embedName)
			continue
		}
		want, err := os.ReadFile(src)
		if err != nil {
			t.Skipf("canonical %s not present (%v) -- skipping drift check", src, err)
			continue
		}
		if string(want) != string(embedded) {
			t.Errorf("embedded ptp/%s has drifted from %s -- run `make sync-provision` and re-commit", f.embedName, src)
		}
		// The one way heredoc embedding can silently truncate: a payload
		// containing the delimiter line.
		if strings.Contains(string(embedded), ptpHeredocEOF) {
			t.Errorf("ptp/%s contains the heredoc delimiter %q -- pick a new delimiter", f.embedName, ptpHeredocEOF)
		}
	}
}
