package main

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

// fakeSystemd puts a stand-in `systemctl` (and a pass-through `sudo`) first on
// PATH. Unit state lives as marker files in the returned dir: NAME.loaded,
// NAME.active, and NAME.stuck (stop "succeeds" but the unit stays active).
// Every systemctl invocation is appended to dir/calls.
func fakeSystemd(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	systemctl := `#!/bin/sh
D="` + dir + `"
echo "$*" >> "$D/calls"
case "$1" in
  show)      # show -p LoadState --value UNIT
    if [ -e "$D/$5.loaded" ]; then echo loaded; else echo not-found; fi ;;
  is-active) # is-active --quiet UNIT
    [ -e "$D/$3.active" ] ;;
  stop)
    [ -e "$D/$2.stuck" ] || rm -f "$D/$2.active" ;;
  start)
    touch "$D/$2.active" ;;
  *) exit 1 ;;
esac
`
	sudo := "#!/bin/sh\nexec \"$@\"\n"
	for name, body := range map[string]string{"systemctl": systemctl, "sudo": sudo} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	return dir
}

func markUnit(t *testing.T, dir, unit string, states ...string) {
	t.Helper()
	for _, s := range states {
		if err := os.WriteFile(filepath.Join(dir, unit+"."+s), nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func calls(t *testing.T, dir string) []string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(dir, "calls"))
	if err != nil {
		return nil
	}
	return strings.Split(strings.TrimSpace(string(b)), "\n")
}

func hasCall(cs []string, want string) bool {
	for _, c := range cs {
		if c == want {
			return true
		}
	}
	return false
}

// The rpi500 case: a windowed dev box updating dlsh. dserv is running;
// stim2.service is installed but disabled and inactive. Only dserv may be
// stopped -- and therefore only dserv is restarted afterwards.
func TestStopForInstallSkipsLoadedButInactive(t *testing.T) {
	dir := fakeSystemd(t)
	markUnit(t, dir, "dserv", "loaded", "active")
	markUnit(t, dir, "stim2", "loaded") // installed by the stim2 package, not running

	stopped, err := stopForInstall([]string{"dserv", "stim2"})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !reflect.DeepEqual(stopped, []string{"dserv"}) {
		t.Fatalf("stopped = %v, want [dserv] -- anything else gets STARTED after the install", stopped)
	}
	cs := calls(t, dir)
	if hasCall(cs, "stop stim2") {
		t.Errorf("stim2 was not running and must not be touched; calls: %v", cs)
	}
	if !hasCall(cs, "stop dserv") {
		t.Errorf("dserv was running and must be stopped; calls: %v", cs)
	}
}

// An in-cage box: everything running, everything stopped (and so restarted) --
// the fix must not change behaviour where stim2.service is the real display.
func TestStopForInstallStopsEverythingRunning(t *testing.T) {
	dir := fakeSystemd(t)
	markUnit(t, dir, "dserv", "loaded", "active")
	markUnit(t, dir, "stim2", "loaded", "active")

	stopped, err := stopForInstall([]string{"dserv", "stim2"})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !reflect.DeepEqual(stopped, []string{"dserv", "stim2"}) {
		t.Fatalf("stopped = %v, want [dserv stim2]", stopped)
	}
}

// A display box has no dserv unit at all: skipped, not an error.
func TestStopForInstallSkipsMissingUnit(t *testing.T) {
	dir := fakeSystemd(t)
	markUnit(t, dir, "stim2", "loaded", "active")

	stopped, err := stopForInstall([]string{"dserv", "stim2"})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !reflect.DeepEqual(stopped, []string{"stim2"}) {
		t.Fatalf("stopped = %v, want [stim2]", stopped)
	}
}

// A unit that survives `systemctl stop` must still refuse the install, and
// report what it had already stopped.
func TestStopForInstallRefusesWhenStopDoesNotTake(t *testing.T) {
	dir := fakeSystemd(t)
	markUnit(t, dir, "dserv", "loaded", "active")
	markUnit(t, dir, "stim2", "loaded", "active", "stuck")

	stopped, err := stopForInstall([]string{"dserv", "stim2"})
	if err == nil || !strings.Contains(err.Error(), "stim2 is still active") {
		t.Fatalf("err = %v, want refusal naming stim2", err)
	}
	if !reflect.DeepEqual(stopped, []string{"dserv"}) {
		t.Fatalf("stopped = %v, want [dserv]", stopped)
	}
}
