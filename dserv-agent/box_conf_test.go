package main

import (
	"os"
	"path/filepath"
	"testing"
)

// A box.conf as step_record_identity writes it: comments, blank lines, and
// empty values for fields the bootstrap had no answer for (role, stim_mode
// on a cage box).
func TestReadBoxIdentity(t *testing.T) {
	path := filepath.Join(t.TempDir(), "box.conf")
	conf := `# Declared identity for this box -- written by the dserv bootstrap.
# Retype: curl -sSL https://dserv.net/setup?profile=<name> | bash -s -- --skip-scripts

profile=dev
stim_mode=windowed
components=dserv,stim2,dlsh,dserv-agent
workgroup=sheinberg
registry=https://dserv.net
role=
provisioned=2026-08-13T12:00:00Z
`
	if err := os.WriteFile(path, []byte(conf), 0644); err != nil {
		t.Fatal(err)
	}

	id := readBoxIdentity(path)
	if id == nil {
		t.Fatal("readBoxIdentity returned nil for a populated file")
	}
	if id.Profile != "dev" {
		t.Errorf("Profile = %q, want dev", id.Profile)
	}
	if id.StimMode != "windowed" {
		t.Errorf("StimMode = %q, want windowed", id.StimMode)
	}
	if id.Components != "dserv,stim2,dlsh,dserv-agent" {
		t.Errorf("Components = %q", id.Components)
	}
	if id.Workgroup != "sheinberg" {
		t.Errorf("Workgroup = %q, want sheinberg", id.Workgroup)
	}
	if id.Registry != "https://dserv.net" {
		t.Errorf("Registry = %q", id.Registry)
	}
	if id.Role != "" {
		t.Errorf("Role = %q, want empty", id.Role)
	}
	if id.Provisioned != "2026-08-13T12:00:00Z" {
		t.Errorf("Provisioned = %q", id.Provisioned)
	}
}

// Absent file and value-free file both mean "pre-identity box": the status
// JSON must omit the section (nil), not serve an empty one the panel would
// render as a recorded-but-blank profile.
func TestReadBoxIdentityAbsentOrEmpty(t *testing.T) {
	if id := readBoxIdentity(filepath.Join(t.TempDir(), "nope.conf")); id != nil {
		t.Errorf("missing file: got %+v, want nil", id)
	}

	path := filepath.Join(t.TempDir(), "box.conf")
	if err := os.WriteFile(path, []byte("# only comments\nprofile=\nrole=\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if id := readBoxIdentity(path); id != nil {
		t.Errorf("value-free file: got %+v, want nil", id)
	}
}

// Unrecognized keys must not count as content: a future bootstrap writing
// extra fields should not turn an otherwise-empty identity into a recorded
// one on an old agent, and hand-added junk lines are ignored.
func TestReadBoxIdentityUnknownKeys(t *testing.T) {
	path := filepath.Join(t.TempDir(), "box.conf")
	if err := os.WriteFile(path, []byte("future_key=abc\nnot a kv line\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if id := readBoxIdentity(path); id != nil {
		t.Errorf("unknown-keys-only file: got %+v, want nil", id)
	}
}
