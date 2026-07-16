package main

import (
	"path/filepath"
	"testing"
)

func newTestRegistry(t *testing.T) *ESSRegistry {
	t.Helper()
	dbPath := filepath.Join(t.TempDir(), "test.db")
	reg, err := NewESSRegistry(dbPath)
	if err != nil {
		t.Fatalf("NewESSRegistry: %v", err)
	}
	if err := reg.migrateConfigsTables(); err != nil {
		t.Fatalf("migrateConfigsTables: %v", err)
	}
	return reg
}

func TestSubjectsCRUD(t *testing.T) {
	reg := newTestRegistry(t)
	const wg = "brown-sheinberg"

	// create (name lowercased), plus an inactive one
	if _, err := reg.CreateSubject(&ESSSubject{Workgroup: wg, Name: "Riker", Species: "macaque", Active: true}); err != nil {
		t.Fatalf("create riker: %v", err)
	}
	if _, err := reg.CreateSubject(&ESSSubject{Workgroup: wg, Name: "momo", Active: false}); err != nil {
		t.Fatalf("create momo: %v", err)
	}

	// same name is allowed in a DIFFERENT workgroup (workgroup-scoped)
	if _, err := reg.CreateSubject(&ESSSubject{Workgroup: "other-lab", Name: "riker", Active: true}); err != nil {
		t.Fatalf("create riker in other-lab: %v", err)
	}

	// duplicate (workgroup, name) -> UNIQUE error
	if _, err := reg.CreateSubject(&ESSSubject{Workgroup: wg, Name: "riker", Active: true}); err == nil {
		t.Fatalf("expected UNIQUE error on duplicate riker")
	}

	// active-only list = [riker]; momo excluded
	active, err := reg.ListSubjects(wg, false)
	if err != nil {
		t.Fatalf("list active: %v", err)
	}
	if len(active) != 1 || active[0].Name != "riker" {
		t.Fatalf("active list = %v, want [riker]", names(active))
	}

	// include-inactive, ordered by name = [momo, riker]
	all, err := reg.ListSubjects(wg, true)
	if err != nil {
		t.Fatalf("list all: %v", err)
	}
	if got := names(all); len(got) != 2 || got[0] != "momo" || got[1] != "riker" {
		t.Fatalf("all list = %v, want [momo riker]", got)
	}

	// Get is case-insensitive on name (lowercased)
	s, err := reg.GetSubject(wg, "RIKER")
	if err != nil || s == nil {
		t.Fatalf("get RIKER: %v (nil=%v)", err, s == nil)
	}
	if s.Name != "riker" || s.Species != "macaque" || !s.Active {
		t.Fatalf("get riker = %+v", s)
	}

	// update: reactivate momo, set species
	momo, _ := reg.GetSubject(wg, "momo")
	momo.Active = true
	momo.Species = "human"
	if err := reg.UpdateSubject(momo); err != nil {
		t.Fatalf("update momo: %v", err)
	}
	if reactivated, _ := reg.ListSubjects(wg, false); len(reactivated) != 2 {
		t.Fatalf("after reactivate, active count = %d, want 2", len(reactivated))
	}

	// delete + not-found on second delete
	if err := reg.DeleteSubject(wg, "riker"); err != nil {
		t.Fatalf("delete riker: %v", err)
	}
	if err := reg.DeleteSubject(wg, "riker"); err == nil {
		t.Fatalf("expected not-found on second delete")
	}

	// other-lab's riker is untouched (workgroup isolation)
	if s, _ := reg.GetSubject("other-lab", "riker"); s == nil {
		t.Fatalf("other-lab riker should still exist")
	}
}

func names(subs []*ESSSubject) []string {
	out := make([]string, len(subs))
	for i, s := range subs {
		out[i] = s.Name
	}
	return out
}
