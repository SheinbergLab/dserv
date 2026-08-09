package main

import (
	"reflect"
	"testing"
)

// The agent must update last: its install restarts it, and runComponentInstall
// aborts the whole batch if a request fails, so anything queued behind it races
// the restart. This came out right before agentLast existed, but only because
// deps sort first and dserv-agent sits late in components.json -- the cases
// below are the ones a reordering of that file would have broken silently.
func TestAgentLast(t *testing.T) {
	cases := []struct {
		name string
		in   []string
		want []string
	}{
		{"already last", []string{"dlsh", "dserv", "dserv-agent"}, []string{"dlsh", "dserv", "dserv-agent"}},
		{"first", []string{"dserv-agent", "dlsh", "dserv"}, []string{"dlsh", "dserv", "dserv-agent"}},
		{"middle", []string{"dlsh", "dserv-agent", "stim2"}, []string{"dlsh", "stim2", "dserv-agent"}},
		{"only agent", []string{"dserv-agent"}, []string{"dserv-agent"}},
		{"absent", []string{"dlsh", "stim2"}, []string{"dlsh", "stim2"}},
		{"empty", []string{}, []string{}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := agentLast(c.in); !reflect.DeepEqual(got, c.want) {
				t.Errorf("agentLast(%v) = %v, want %v", c.in, got, c.want)
			}
		})
	}
}
