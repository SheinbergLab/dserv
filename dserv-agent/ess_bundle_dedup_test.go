package main

// Tests for the bundle push dedup: a content-identical push must be
// acknowledged without writes (no history row, no lastPushedAt advance),
// while any real change must import normally. The signature must be
// blind to volatile fields — ids, timestamps, sourceRig, useCount —
// because that is exactly what differs between two boxes' exports of
// the same content.

import (
	"encoding/json"
	"testing"
	"time"
)

func testBundle() *ESSProjectBundle {
	return &ESSProjectBundle{
		Project: ESSProjectDef{
			Name:        "ricochet",
			Description: "bounce-point extrapolation",
			Systems:     []string{"planko"},
		},
		Configs: []ESSConfig{
			{
				ID:          30, // per-box row id: must not participate
				Name:        "ricochet-adapt",
				Description: "unsignalled bounciness change points",
				System:      "planko",
				Protocol:    "ricochet",
				Variant:     "adapt",
				Subject:     "human",
				VariantArgs: map[string]interface{}{"n_trials": float64(120), "restitutions": "0.10 0.30"},
				Params:      map[string]interface{}{"juice_ml": 0.4, "fix_radius": float64(3)},
				Tags:        []string{"ricochet"},
				CreatedBy:   "root",
				UseCount:    7,
			},
		},
		ExportedAt: time.Now(),
		ExportedBy: "david",
		SourceRig:  "MacBook Air (2)",
	}
}

func bundleHistoryCount(t *testing.T, reg *ESSRegistry, project string) int {
	t.Helper()
	var n int
	if err := reg.db.QueryRow(
		"SELECT COUNT(*) FROM ess_bundle_history WHERE project_name = ?", project).Scan(&n); err != nil {
		t.Fatalf("history count: %v", err)
	}
	return n
}

func TestBundlePushDedup(t *testing.T) {
	reg := newTestRegistry(t)
	const wg = "brown-sheinberg"

	// First push: imports normally.
	res, err := reg.ImportProjectBundle(wg, testBundle(), true)
	if err != nil {
		t.Fatalf("first push: %v", err)
	}
	if res.Unchanged {
		t.Fatal("first push must not be reported unchanged")
	}
	if n := bundleHistoryCount(t, reg, "ricochet"); n != 1 {
		t.Fatalf("history rows after first push: %d, want 1", n)
	}

	// Round-trip through JSON like a real push body (numbers -> float64).
	rt := func(b *ESSProjectBundle) *ESSProjectBundle {
		raw, _ := json.Marshal(b)
		var out ESSProjectBundle
		if err := json.Unmarshal(raw, &out); err != nil {
			t.Fatalf("roundtrip: %v", err)
		}
		return &out
	}

	// Second push: same content from a "different box" — new ids, rig,
	// use counts, exporter. Must dedup.
	b2 := rt(testBundle())
	b2.SourceRig = "rpi500"
	b2.ExportedBy = "unknown"
	b2.Configs[0].ID = 9999
	b2.Configs[0].UseCount = 42
	res, err = reg.ImportProjectBundle(wg, b2, true)
	if err != nil {
		t.Fatalf("second push: %v", err)
	}
	if !res.Unchanged {
		t.Fatal("content-identical push was not deduped")
	}
	if n := bundleHistoryCount(t, reg, "ricochet"); n != 1 {
		t.Fatalf("history rows after no-op push: %d, want 1 (no new row)", n)
	}

	// Third push: a real parameter change must import.
	b3 := rt(testBundle())
	b3.Configs[0].Params["juice_ml"] = 0.5
	res, err = reg.ImportProjectBundle(wg, b3, true)
	if err != nil {
		t.Fatalf("third push: %v", err)
	}
	if res.Unchanged {
		t.Fatal("changed content was wrongly deduped")
	}
	if n := bundleHistoryCount(t, reg, "ricochet"); n != 2 {
		t.Fatalf("history rows after real change: %d, want 2", n)
	}

	// Fourth: repush the changed content — dedups again.
	res, err = reg.ImportProjectBundle(wg, rt(b3), true)
	if err != nil {
		t.Fatalf("fourth push: %v", err)
	}
	if !res.Unchanged {
		t.Fatal("repush of current content was not deduped")
	}
}

func TestBundleSignatureQueueIdentity(t *testing.T) {
	// Queue items reference configs by per-box row id; the signature must
	// compare them by NAME so two boxes' exports of the same queue match.
	a := testBundle()
	a.Queues = []ESSQueue{{
		Name: "daily", AutoStart: true,
		Items: []ESSQueueItem{{ConfigID: 30, Position: 0, RepeatCount: 2}},
	}}
	b := testBundle()
	b.Configs[0].ID = 555
	b.Queues = []ESSQueue{{
		Name: "daily", AutoStart: true,
		Items: []ESSQueueItem{{ConfigID: 555, Position: 0, RepeatCount: 2}},
	}}
	if bundleContentSignature(a) != bundleContentSignature(b) {
		t.Fatal("queue item config-id difference leaked into signature")
	}

	c := testBundle()
	c.Configs[0].ID = 555
	c.Queues = []ESSQueue{{
		Name: "daily", AutoStart: true,
		Items: []ESSQueueItem{{ConfigID: 555, Position: 0, RepeatCount: 3}},
	}}
	if bundleContentSignature(a) == bundleContentSignature(c) {
		t.Fatal("repeatCount change did not change signature")
	}
}
