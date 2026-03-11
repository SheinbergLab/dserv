package main

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Config holds all configuration for dservctl
type Config struct {
	Host      string // Target host
	AgentPort int    // Agent HTTP port
	Token     string // Bearer token for agent API
	Workgroup string // Default workgroup
	User      string // Username for registry operations
	Registry  string // Registry URL (e.g., https://dserv.net)
	JSON      bool   // JSON output mode
	Verbose   bool   // Verbose logging
	Command   string // -c command (single execution)
}

// LoadConfig creates a Config with defaults, then overlays config file and env vars.
func LoadConfig() *Config {
	cfg := &Config{
		Host:      "localhost",
		AgentPort: 80,
	}

	// Layer 1: config file
	cfg.loadConfigFile()

	// Layer 2: environment variables
	cfg.loadEnvVars()

	return cfg
}

// ParseFlags extracts global flags from args and returns remaining args.
// This is Layer 3 (highest priority).
func (cfg *Config) ParseFlags(args []string) []string {
	var remaining []string
	i := 0
	for i < len(args) {
		switch args[i] {
		case "-H", "--host":
			if i+1 < len(args) {
				cfg.Host = args[i+1]
				i += 2
			} else {
				fmt.Fprintf(os.Stderr, "Error: %s requires a value\n", args[i])
				os.Exit(2)
			}
		case "--agent-port":
			if i+1 < len(args) {
				port, err := strconv.Atoi(args[i+1])
				if err != nil {
					fmt.Fprintf(os.Stderr, "Error: invalid port: %s\n", args[i+1])
					os.Exit(2)
				}
				cfg.AgentPort = port
				i += 2
			} else {
				fmt.Fprintf(os.Stderr, "Error: --agent-port requires a value\n")
				os.Exit(2)
			}
		case "-t", "--token":
			if i+1 < len(args) {
				cfg.Token = args[i+1]
				i += 2
			} else {
				fmt.Fprintf(os.Stderr, "Error: %s requires a value\n", args[i])
				os.Exit(2)
			}
		case "-w", "--workgroup":
			if i+1 < len(args) {
				cfg.Workgroup = args[i+1]
				i += 2
			} else {
				fmt.Fprintf(os.Stderr, "Error: %s requires a value\n", args[i])
				os.Exit(2)
			}
		case "-u", "--user":
			if i+1 < len(args) {
				cfg.User = args[i+1]
				i += 2
			} else {
				fmt.Fprintf(os.Stderr, "Error: %s requires a value\n", args[i])
				os.Exit(2)
			}
		case "--registry", "-r":
			if i+1 < len(args) {
				cfg.Registry = args[i+1]
				i += 2
			} else {
				fmt.Fprintf(os.Stderr, "Error: %s requires a value\n", args[i])
				os.Exit(2)
			}
		case "-c":
			if i+1 < len(args) {
				cfg.Command = args[i+1]
				i += 2
			} else {
				fmt.Fprintf(os.Stderr, "Error: -c requires a command\n")
				os.Exit(2)
			}
		case "--json":
			cfg.JSON = true
			i++
		case "--verbose":
			cfg.Verbose = true
			i++
		case "--help", "-h":
			printUsage()
			os.Exit(0)
		default:
			// Not a global flag — stop parsing flags
			remaining = append(remaining, args[i:]...)
			return remaining
		}
	}
	return remaining
}

// loadConfigFile reads ~/.dservctl if it exists.
// Format: key: value (one per line, # comments)
func (cfg *Config) loadConfigFile() {
	home, err := os.UserHomeDir()
	if err != nil {
		return
	}
	path := filepath.Join(home, ".dservctl")
	f, err := os.Open(path)
	if err != nil {
		return
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		key, value, found := strings.Cut(line, ":")
		if !found {
			continue
		}
		key = strings.TrimSpace(key)
		value = strings.TrimSpace(value)

		switch key {
		case "host":
			cfg.Host = value
		case "agent-port":
			if port, err := strconv.Atoi(value); err == nil {
				cfg.AgentPort = port
			}
		case "token":
			cfg.Token = value
		case "workgroup":
			cfg.Workgroup = value
		case "user":
			cfg.User = value
		case "registry":
			cfg.Registry = value
		}
	}
}

// loadEnvVars overlays environment variables
func (cfg *Config) loadEnvVars() {
	if v := os.Getenv("DSERV_HOST"); v != "" {
		cfg.Host = v
	}
	if v := os.Getenv("DSERV_AGENT_PORT"); v != "" {
		if port, err := strconv.Atoi(v); err == nil {
			cfg.AgentPort = port
		}
	}
	if v := os.Getenv("DSERV_AGENT_TOKEN"); v != "" {
		cfg.Token = v
	}
	if v := os.Getenv("DSERV_WORKGROUP"); v != "" {
		cfg.Workgroup = v
	}
	if v := os.Getenv("DSERV_USER"); v != "" {
		cfg.User = v
	}
	if v := os.Getenv("DSERV_REGISTRY"); v != "" {
		cfg.Registry = v
	}
}

// AgentBaseURL returns the base URL for agent HTTP API
func (cfg *Config) AgentBaseURL() string {
	return fmt.Sprintf("http://%s:%d", cfg.Host, cfg.AgentPort)
}

// RegistryBaseURL returns the base URL for the ESS registry API.
// Uses --registry flag, DSERV_REGISTRY env, config file, or auto-discovers
// from the ess/registry/url datapoint.
func (cfg *Config) RegistryBaseURL() string {
	if cfg.Registry != "" {
		return strings.TrimRight(cfg.Registry, "/")
	}

	// Try to discover from dserv datapoint
	resp, err := SendToDserv(cfg.Host, "dservGet ess/registry/url")
	if err == nil {
		resp = strings.TrimSpace(resp)
		if resp != "" && !strings.HasPrefix(resp, "!") {
			if cfg.Verbose {
				fmt.Fprintf(os.Stderr, "Discovered registry URL: %s\n", resp)
			}
			return strings.TrimRight(resp, "/")
		}
	}

	// Fall back to local agent
	return cfg.AgentBaseURL()
}
