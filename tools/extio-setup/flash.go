// flash.go -- put a box in BOOTSEL and copy a UF2 to its mass-storage drive.
//
// Works for a connected box (we send `bootsel` first) and for a blank or
// already-BOOTSEL'd board (the RPI-RP2 drive is simply already there).
// The RP2 reboots the instant the last UF2 block lands, which can surface
// as a write/close error even though flashing succeeded -- so success is
// judged by the drive vanishing and the extio CDC device reappearing.
package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"time"
)

func (s *server) flash(uf2 string) ([]string, error) {
	var steps []string
	step := func(format string, a ...any) {
		steps = append(steps, fmt.Sprintf(format, a...))
	}

	// 1. If a box is connected, ask it to reboot into BOOTSEL.
	if l := s.current(); l != nil {
		lines, err := l.Exec("bootsel", 300*time.Millisecond, 3*time.Second)
		if err != nil {
			step("bootsel command: %v (continuing; board may already be in BOOTSEL)", err)
		} else {
			step("bootsel: %s", firstOr(lines, "(no response)"))
		}
		l.Close()
		s.mu.Lock()
		s.link = nil
		if s.data != nil {
			s.data.Close()
			s.data = nil
		}
		s.mu.Unlock()
	}

	// 2. Wait for the BOOTSEL drive.
	vol, err := waitForVolume(25 * time.Second)
	if err != nil {
		return steps, err
	}
	step("BOOTSEL drive: %s", vol)

	// 3. Copy the UF2 (bounded: a stale mount can block writes forever).
	type copyRes struct {
		n   int64
		err error
	}
	ch := make(chan copyRes, 1)
	go func() {
		n, err := copyUF2(uf2, filepath.Join(vol, filepath.Base(uf2)))
		ch <- copyRes{n, err}
	}()
	select {
	case r := <-ch:
		if r.err != nil {
			return steps, fmt.Errorf("copy: %w", r.err)
		}
		step("copied %s (%d bytes)", filepath.Base(uf2), r.n)
	case <-time.After(90 * time.Second):
		return steps, fmt.Errorf("copy stalled -- stale BOOTSEL mount; unplug the board and replug with BOOTSEL held")
	}

	// 4. The board reboots itself; wait for the drive to vanish and the
	// extio CDC console to come back.
	waitGone(vol, 10*time.Second)
	port, err := waitForConsole(15 * time.Second)
	if err != nil {
		return steps, fmt.Errorf("board did not re-enumerate: %w", err)
	}
	step("re-enumerated: %s", port)
	return steps, nil
}

func firstOr(lines []string, def string) string {
	if len(lines) > 0 {
		return lines[0]
	}
	return def
}

// bootselVolume returns the mounted RP2 BOOTSEL drive, if any. The volume
// is confirmed by its INFO_UF2.TXT marker, not just its name.
func bootselVolume() string {
	var roots []string
	switch runtime.GOOS {
	case "darwin":
		roots = []string{"/Volumes"}
	case "linux":
		roots = []string{"/media", "/run/media"}
		if home, err := os.UserHomeDir(); err == nil {
			roots = append(roots, filepath.Join("/media", filepath.Base(home)))
		}
	default:
		return ""
	}
	for _, root := range roots {
		matches, _ := filepath.Glob(filepath.Join(root, "*"))
		if runtime.GOOS == "linux" {
			deeper, _ := filepath.Glob(filepath.Join(root, "*", "*"))
			matches = append(matches, deeper...)
		}
		for _, m := range matches {
			if statBounded(filepath.Join(m, "INFO_UF2.TXT"), 2*time.Second) == nil {
				return m
			}
		}
	}
	return ""
}

func waitForVolume(timeout time.Duration) (string, error) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if v := bootselVolume(); v != "" {
			return v, nil
		}
		time.Sleep(500 * time.Millisecond)
	}
	return "", fmt.Errorf("no BOOTSEL drive appeared within %s (hold BOOTSEL while plugging in a blank board)", timeout)
}

func waitGone(vol string, timeout time.Duration) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if statBounded(filepath.Join(vol, "INFO_UF2.TXT"), 2*time.Second) != nil {
			return
		}
		time.Sleep(300 * time.Millisecond)
	}
}

// statBounded is os.Stat with a deadline: a wedged FSKit/msdos BOOTSEL mount
// can block stat() indefinitely (seen live on macOS). Timeout reads as "not
// there"; the blocked goroutine is abandoned and dies when the mount clears.
func statBounded(path string, d time.Duration) error {
	done := make(chan error, 1)
	go func() {
		_, err := os.Stat(path)
		done <- err
	}()
	select {
	case err := <-done:
		return err
	case <-time.After(d):
		return fmt.Errorf("%s: filesystem not responding", path)
	}
}

func waitForConsole(timeout time.Duration) (string, error) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if ports, err := listBoxPorts(); err == nil && len(ports) > 0 {
			return ports[0].Console, nil
		}
		time.Sleep(500 * time.Millisecond)
	}
	return "", fmt.Errorf("no extio CDC device within %s", timeout)
}

// copyUF2 writes the image in chunks. A failure on the final stretch (or on
// close/sync) is expected: the board reboots mid-write once the last block
// is flashed. Those are reported as success; the caller verifies by
// re-enumeration anyway.
func copyUF2(src, dst string) (int64, error) {
	in, err := os.Open(src)
	if err != nil {
		return 0, err
	}
	defer in.Close()
	fi, err := in.Stat()
	if err != nil {
		return 0, err
	}
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return 0, err
	}
	n, cerr := io.Copy(out, in)
	serr := out.Sync()
	xerr := out.Close()
	if n == fi.Size() {
		return n, nil // fully written; reboot-induced sync/close errors are fine
	}
	for _, e := range []error{cerr, serr, xerr} {
		if e != nil {
			return n, e
		}
	}
	return n, nil
}
