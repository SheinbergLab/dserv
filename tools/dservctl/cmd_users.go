package main

import (
	"fmt"
	"os"
	"strings"
)

func runUsers(cfg *Config, args []string) int {
	if len(args) == 0 {
		printUsersUsage()
		return 2
	}

	switch args[0] {
	case "list", "ls":
		return runUsersList(cfg, args[1:])
	case "get":
		return runUsersGet(cfg, args[1:])
	case "add":
		return runUsersAdd(cfg, args[1:])
	case "remove", "rm", "delete":
		return runUsersRemove(cfg, args[1:])
	default:
		PrintError("unknown users subcommand %q (use list, get, add, or remove)", args[0])
		return 2
	}
}

func printUsersUsage() {
	fmt.Fprintf(os.Stderr, "Usage:\n")
	fmt.Fprintf(os.Stderr, "  dservctl users list                                          List users in workgroup\n")
	fmt.Fprintf(os.Stderr, "  dservctl users get <username>                                Show one user\n")
	fmt.Fprintf(os.Stderr, "  dservctl users add <username> [--name N] [--email E] [--role R]  Add or update a user\n")
	fmt.Fprintf(os.Stderr, "  dservctl users remove <username>                             Remove a user\n")
	fmt.Fprintf(os.Stderr, "\nRoles: admin, editor (default), viewer\n")
}

func runUsersList(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	client := NewRegistryClient(cfg)
	users, err := client.ListUsers(cfg.Workgroup)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(users)
		return 0
	}

	if len(users) == 0 {
		fmt.Printf("No users in workgroup %q.\n", cfg.Workgroup)
		return 0
	}

	headers := []string{"USERNAME", "FULL NAME", "EMAIL", "ROLE"}
	var rows [][]string
	for _, u := range users {
		rows = append(rows, []string{
			strVal(u, "username"),
			strVal(u, "fullName"),
			strVal(u, "email"),
			strVal(u, "role"),
		})
	}
	PrintTable(headers, rows)
	return 0
}

func runUsersGet(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}
	if len(args) < 1 {
		PrintError("usage: dservctl users get <username>")
		return 2
	}
	username := args[0]

	client := NewRegistryClient(cfg)
	user, err := client.GetUser(cfg.Workgroup, username)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(user)
		return 0
	}

	fmt.Printf("Workgroup: %s\n", strVal(user, "workgroup"))
	fmt.Printf("Username:  %s\n", strVal(user, "username"))
	fmt.Printf("Full name: %s\n", strVal(user, "fullName"))
	fmt.Printf("Email:     %s\n", strVal(user, "email"))
	fmt.Printf("Role:      %s\n", strVal(user, "role"))
	return 0
}

func runUsersAdd(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}
	if len(args) < 1 || strings.HasPrefix(args[0], "-") {
		PrintError("usage: dservctl users add <username> [--name N] [--email E] [--role R]")
		return 2
	}
	username := args[0]

	fullName := ""
	email := ""
	role := ""
	for i := 1; i < len(args); i++ {
		switch args[i] {
		case "--name", "-n":
			if i+1 < len(args) {
				fullName = args[i+1]
				i++
			}
		case "--email", "-e":
			if i+1 < len(args) {
				email = args[i+1]
				i++
			}
		case "--role", "-r":
			if i+1 < len(args) {
				role = args[i+1]
				i++
			}
		default:
			PrintError("unknown flag: %s", args[i])
			return 2
		}
	}

	client := NewRegistryClient(cfg)
	result, err := client.SaveUser(cfg.Workgroup, username, fullName, email, role)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	fmt.Printf("Added %s to %s", username, cfg.Workgroup)
	if role != "" {
		fmt.Printf(" (role: %s)", role)
	}
	fmt.Println()
	return 0
}

func runUsersRemove(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}
	if len(args) < 1 {
		PrintError("usage: dservctl users remove <username>")
		return 2
	}
	username := args[0]

	client := NewRegistryClient(cfg)
	if _, err := client.DeleteUser(cfg.Workgroup, username); err != nil {
		PrintError("%v", err)
		return 1
	}

	fmt.Printf("Removed %s from %s\n", username, cfg.Workgroup)
	return 0
}
