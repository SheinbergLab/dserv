package main

import (
	"os"
	"path/filepath"
	"strings"
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
time_role=client eth0
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
	if id.TimeRole != "client eth0" {
		t.Errorf("TimeRole = %q, want \"client eth0\"", id.TimeRole)
	}
}

// The write-back after a panel apply/disable: surgical line replace,
// comments preserved byte-for-byte, append for pre-time_role files, and a
// plain error (not a created file) for unrecorded boxes.
func TestUpdateBoxConfTimeRole(t *testing.T) {
	path := filepath.Join(t.TempDir(), "box.conf")
	orig := "# a comment worth preserving\nprofile=stim\ntime_role=ptp-client old\nrole=x\n"
	if err := os.WriteFile(path, []byte(orig), 0644); err != nil {
		t.Fatal(err)
	}
	if err := updateBoxConfTimeRole(path, "ntp-client 192.168.88.29"); err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(path)
	want := "# a comment worth preserving\nprofile=stim\ntime_role=ntp-client 192.168.88.29\nrole=x\n"
	if string(got) != want {
		t.Errorf("replace: got %q, want %q", got, want)
	}

	// disable writes empty
	if err := updateBoxConfTimeRole(path, ""); err != nil {
		t.Fatal(err)
	}
	got, _ = os.ReadFile(path)
	if !strings.Contains(string(got), "\ntime_role=\n") {
		t.Errorf("disable: time_role not emptied: %q", got)
	}

	// pre-time_role file: appended before the trailing blank line
	path2 := filepath.Join(t.TempDir(), "box.conf")
	os.WriteFile(path2, []byte("profile=dev\nrole=\n"), 0644)
	if err := updateBoxConfTimeRole(path2, "client eth0"); err != nil {
		t.Fatal(err)
	}
	got, _ = os.ReadFile(path2)
	if !strings.Contains(string(got), "time_role=client eth0") {
		t.Errorf("append: %q", got)
	}

	// unrecorded box: error, no file created
	missing := filepath.Join(t.TempDir(), "nope.conf")
	if err := updateBoxConfTimeRole(missing, "client eth0"); !os.IsNotExist(err) {
		t.Errorf("missing file: err = %v, want IsNotExist", err)
	}
	if _, err := os.Stat(missing); err == nil {
		t.Error("missing file: write-back created a file it should not have")
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
