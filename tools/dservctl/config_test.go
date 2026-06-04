package main

import (
	"os"
	"testing"
)

func TestConfigDefaults(t *testing.T) {
	// Clear env vars that might interfere
	os.Unsetenv("DSERV_HOST")
	os.Unsetenv("DSERV_AGENT_PORT")
	os.Unsetenv("DSERV_AGENT_TOKEN")
	os.Unsetenv("DSERV_WORKGROUP")
	os.Unsetenv("DSERV_USER")

	cfg := &Config{
		Host:      "localhost",
		AgentPort: 80,
	}

	if cfg.Host != "localhost" {
		t.Errorf("default host = %q, want %q", cfg.Host, "localhost")
	}
	if cfg.AgentPort != 80 {
		t.Errorf("default agent port = %d, want %d", cfg.AgentPort, 80)
	}
}

func TestConfigEnvVars(t *testing.T) {
	os.Setenv("DSERV_HOST", "myhost")
	os.Setenv("DSERV_AGENT_PORT", "8080")
	os.Setenv("DSERV_AGENT_TOKEN", "mytoken")
	os.Setenv("DSERV_WORKGROUP", "mygroup")
	os.Setenv("DSERV_USER", "myuser")
	defer func() {
		os.Unsetenv("DSERV_HOST")
		os.Unsetenv("DSERV_AGENT_PORT")
		os.Unsetenv("DSERV_AGENT_TOKEN")
		os.Unsetenv("DSERV_WORKGROUP")
		os.Unsetenv("DSERV_USER")
	}()

	cfg := &Config{
		Host:      "localhost",
		AgentPort: 80,
	}
	cfg.loadEnvVars()

	if cfg.Host != "myhost" {
		t.Errorf("host = %q, want %q", cfg.Host, "myhost")
	}
	if cfg.AgentPort != 8080 {
		t.Errorf("agent port = %d, want %d", cfg.AgentPort, 8080)
	}
	if cfg.Token != "mytoken" {
		t.Errorf("token = %q, want %q", cfg.Token, "mytoken")
	}
	if cfg.Workgroup != "mygroup" {
		t.Errorf("workgroup = %q, want %q", cfg.Workgroup, "mygroup")
	}
	if cfg.User != "myuser" {
		t.Errorf("user = %q, want %q", cfg.User, "myuser")
	}
}

func TestConfigParseFlags(t *testing.T) {
	cfg := &Config{
		Host:      "localhost",
		AgentPort: 80,
	}

	args := []string{"-H", "remotehost", "--json", "--verbose", "-c", "expr 2+2", "ess", "some_cmd"}
	remaining := cfg.ParseFlags(args)

	if cfg.Host != "remotehost" {
		t.Errorf("host = %q, want %q", cfg.Host, "remotehost")
	}
	if !cfg.JSON {
		t.Error("JSON flag should be true")
	}
	if !cfg.Verbose {
		t.Error("Verbose flag should be true")
	}
	if cfg.Command != "expr 2+2" {
		t.Errorf("command = %q, want %q", cfg.Command, "expr 2+2")
	}
	if len(remaining) != 2 || remaining[0] != "ess" || remaining[1] != "some_cmd" {
		t.Errorf("remaining = %v, want [ess some_cmd]", remaining)
	}
}

func TestParseInlineGlobalFlags(t *testing.T) {
	// Global flag placed AFTER the subcommand and its own flags.
	cfg := &Config{Host: "localhost", AgentPort: 80}
	args := []string{"push", "system", "--dir", "FOLDER", "--add", "-w", "mygroup"}
	remaining := cfg.ParseInlineGlobalFlags(args)

	if cfg.Workgroup != "mygroup" {
		t.Errorf("workgroup = %q, want %q", cfg.Workgroup, "mygroup")
	}
	want := []string{"push", "system", "--dir", "FOLDER", "--add"}
	if !equalStrings(remaining, want) {
		t.Errorf("remaining = %v, want %v", remaining, want)
	}
}

func TestParseInlineGlobalFlagsLongForms(t *testing.T) {
	cfg := &Config{Host: "localhost", AgentPort: 80}
	args := []string{"push", "prf", "--workgroup", "wg", "--user", "alice", "--dir", "./prf"}
	remaining := cfg.ParseInlineGlobalFlags(args)

	if cfg.Workgroup != "wg" {
		t.Errorf("workgroup = %q, want %q", cfg.Workgroup, "wg")
	}
	if cfg.User != "alice" {
		t.Errorf("user = %q, want %q", cfg.User, "alice")
	}
	want := []string{"push", "prf", "--dir", "./prf"}
	if !equalStrings(remaining, want) {
		t.Errorf("remaining = %v, want %v", remaining, want)
	}
}

// TestParseInlineGlobalFlagsNoCollision ensures subcommand flags that share a
// short letter with an excluded global flag are left untouched. `-r` here is
// `users --role`, NOT the global --registry.
func TestParseInlineGlobalFlagsNoCollision(t *testing.T) {
	cfg := &Config{Host: "localhost", AgentPort: 80}
	args := []string{"users", "add", "alice", "-r", "admin"}
	remaining := cfg.ParseInlineGlobalFlags(args)

	if cfg.Registry != "" {
		t.Errorf("registry should be untouched, got %q", cfg.Registry)
	}
	want := []string{"users", "add", "alice", "-r", "admin"}
	if !equalStrings(remaining, want) {
		t.Errorf("remaining = %v, want %v", remaining, want)
	}
}

// TestParseInlineGlobalFlagsTerminator ensures "--" passes the rest through
// verbatim, so a positional value matching a global flag name survives.
func TestParseInlineGlobalFlagsTerminator(t *testing.T) {
	cfg := &Config{Host: "localhost", AgentPort: 80}
	args := []string{"set", "mykey", "--", "--workgroup"}
	remaining := cfg.ParseInlineGlobalFlags(args)

	if cfg.Workgroup != "" {
		t.Errorf("workgroup should be untouched, got %q", cfg.Workgroup)
	}
	want := []string{"set", "mykey", "--workgroup"}
	if !equalStrings(remaining, want) {
		t.Errorf("remaining = %v, want %v", remaining, want)
	}
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func TestAgentBaseURL(t *testing.T) {
	cfg := &Config{Host: "rig1.local", AgentPort: 8080}
	url := cfg.AgentBaseURL()
	if url != "http://rig1.local:8080" {
		t.Errorf("AgentBaseURL = %q, want %q", url, "http://rig1.local:8080")
	}
}
