package main

import (
	"net"
	"testing"
	"time"
)

// refreshDirtyBadge is called on the success path of every push and sync, so
// its FAILURE modes are the ones that matter: it must never turn a push that
// already worked into a hang. These pin the three ways it can fail.
//
// The budget is checked against a generous ceiling rather than the exact
// value, so a slow CI box does not make this flaky while still catching the
// thing that would actually hurt -- an unbounded wait.

const badgeCeiling = 6 * time.Second

// The happy path, and the only thing that checks the interp name and the
// command are actually right. SKIPPED unless a dserv is answering here,
// because the port is fixed at DservPort and there is no stub to point at.
//
// It does poke a real running dserv, so be clear about what that costs:
// scripts::dirty is a read-only scan of the systems tree that republishes
// scripts/dirty with whatever is true. It changes no files and no registry
// state, which is the same reason refreshDirtyBadge is safe to fire
// speculatively in the first place.
func TestRefreshDirtyBadgeLiveDservIsFast(t *testing.T) {
	c, err := net.DialTimeout("tcp", "127.0.0.1:2560", 500*time.Millisecond)
	if err != nil {
		t.Skipf("no dserv on 127.0.0.1:%d: %v", DservPort, err)
	}
	c.Close()

	cfg := &Config{Host: "127.0.0.1"}
	start := time.Now()
	refreshDirtyBadge(cfg)
	if d := time.Since(start); d >= dirtyRefreshBudget {
		t.Fatalf("a live dserv took %v, which means the budget expired rather "+
			"than the call completing", d)
	}
}

func TestRefreshDirtyBadgeBlackholedHost(t *testing.T) {
	// 192.0.2.0/24 is TEST-NET-1 (RFC 5737): guaranteed not routed, so the
	// connect neither completes nor is refused -- it hangs until something
	// gives up. Without the budget in refreshDirtyBadge this is a 5-second
	// stall on every push; with it, ~2.
	cfg := &Config{Host: "192.0.2.1"}
	start := time.Now()
	refreshDirtyBadge(cfg)
	d := time.Since(start)
	if d > badgeCeiling {
		t.Fatalf("blackholed host took %v, want < %v", d, badgeCeiling)
	}
	if d > dirtyRefreshBudget+time.Second {
		t.Fatalf("blackholed host took %v, expected the %v budget to cut it short",
			d, dirtyRefreshBudget)
	}
}

func TestRefreshDirtyBadgeUnresolvableHost(t *testing.T) {
	// .invalid is reserved (RFC 2606) and must never resolve. This is the
	// DNS leg: resolveHost used net.LookupHost with no timeout at all, so a
	// wedged resolver hung here ahead of every other budget on the path.
	cfg := &Config{Host: "no-such-rig.invalid"}
	start := time.Now()
	refreshDirtyBadge(cfg)
	if d := time.Since(start); d > badgeCeiling {
		t.Fatalf("unresolvable host took %v, want < %v", d, badgeCeiling)
	}
}

func TestResolveHostUnresolvableIsBounded(t *testing.T) {
	start := time.Now()
	got := resolveHost("no-such-rig.invalid")
	d := time.Since(start)
	if got != "no-such-rig.invalid" {
		t.Fatalf("resolveHost returned %q, want the original name back so that "+
			"DialTimeout reports the error", got)
	}
	if d > ResolveTimeout+time.Second {
		t.Fatalf("resolveHost took %v, want <= ResolveTimeout (%v)", d, ResolveTimeout)
	}
}
