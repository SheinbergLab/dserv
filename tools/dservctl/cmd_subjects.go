package main

import (
	"fmt"
	"os"
	"strings"
)

func runSubjects(cfg *Config, args []string) int {
	if len(args) == 0 {
		printSubjectsUsage()
		return 2
	}

	switch args[0] {
	case "list", "ls":
		return runSubjectsList(cfg, args[1:])
	case "get", "show":
		return runSubjectsGet(cfg, args[1:])
	case "add", "set":
		return runSubjectsAdd(cfg, args[1:])
	case "remove", "rm", "delete":
		return runSubjectsRemove(cfg, args[1:])
	case "seed":
		return runSubjectsSeed(cfg, args[1:])
	default:
		PrintError("unknown subjects subcommand %q (use list, get, add, remove, or seed)", args[0])
		return 2
	}
}

func printSubjectsUsage() {
	fmt.Fprintf(os.Stderr, "Usage:\n")
	fmt.Fprintf(os.Stderr, "  dservctl subjects list [--all]                                        List subjects in workgroup (--all includes inactive)\n")
	fmt.Fprintf(os.Stderr, "  dservctl subjects get <name>                                          Show one subject\n")
	fmt.Fprintf(os.Stderr, "  dservctl subjects add <name> [--display N] [--species S] [--inactive] [--description D]  Add or update a subject\n")
	fmt.Fprintf(os.Stderr, "  dservctl subjects remove <name>                                       Remove a subject\n")
	fmt.Fprintf(os.Stderr, "  dservctl subjects seed                                                Register subjects referenced by existing configs\n")
}

func runSubjectsList(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}
	all := false
	for _, a := range args {
		if a == "--all" || a == "-a" {
			all = true
		} else {
			PrintError("unknown flag: %s", a)
			return 2
		}
	}

	client := NewRegistryClient(cfg)
	subjects, err := client.ListSubjects(cfg.Workgroup, all)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(subjects)
		return 0
	}

	if len(subjects) == 0 {
		fmt.Printf("No subjects in workgroup %q.\n", cfg.Workgroup)
		return 0
	}

	headers := []string{"NAME", "DISPLAY", "SPECIES", "ACTIVE"}
	var rows [][]string
	for _, s := range subjects {
		rows = append(rows, []string{
			strVal(s, "name"),
			strVal(s, "displayName"),
			strVal(s, "species"),
			yesNo(boolVal(s, "active")),
		})
	}
	PrintTable(headers, rows)
	return 0
}

func runSubjectsGet(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}
	if len(args) < 1 {
		PrintError("usage: dservctl subjects get <name>")
		return 2
	}
	name := args[0]

	client := NewRegistryClient(cfg)
	subject, err := client.GetSubject(cfg.Workgroup, name)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(subject)
		return 0
	}

	fmt.Printf("Workgroup:   %s\n", strVal(subject, "workgroup"))
	fmt.Printf("Name:        %s\n", strVal(subject, "name"))
	fmt.Printf("Display:     %s\n", strVal(subject, "displayName"))
	fmt.Printf("Species:     %s\n", strVal(subject, "species"))
	fmt.Printf("Active:      %s\n", yesNo(boolVal(subject, "active")))
	fmt.Printf("Description: %s\n", strVal(subject, "description"))
	return 0
}

func runSubjectsAdd(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}
	if len(args) < 1 || strings.HasPrefix(args[0], "-") {
		PrintError("usage: dservctl subjects add <name> [--display N] [--species S] [--inactive] [--description D]")
		return 2
	}
	name := args[0]

	display := ""
	species := ""
	description := ""
	active := true
	for i := 1; i < len(args); i++ {
		switch args[i] {
		case "--display", "-d":
			if i+1 < len(args) {
				display = args[i+1]
				i++
			}
		case "--species", "-s":
			if i+1 < len(args) {
				species = args[i+1]
				i++
			}
		case "--description":
			if i+1 < len(args) {
				description = args[i+1]
				i++
			}
		case "--inactive":
			active = false
		default:
			PrintError("unknown flag: %s", args[i])
			return 2
		}
	}

	client := NewRegistryClient(cfg)
	result, err := client.SaveSubject(cfg.Workgroup, name, display, species, description, active)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	verb := "Updated"
	if created, _ := result["created"].(bool); created {
		verb = "Added"
	}
	fmt.Printf("%s %s in %s\n", verb, strings.ToLower(name), cfg.Workgroup)
	return 0
}

func runSubjectsRemove(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}
	if len(args) < 1 {
		PrintError("usage: dservctl subjects remove <name>")
		return 2
	}
	name := args[0]

	client := NewRegistryClient(cfg)
	if _, err := client.DeleteSubject(cfg.Workgroup, name); err != nil {
		PrintError("%v", err)
		return 1
	}

	fmt.Printf("Removed %s from %s\n", strings.ToLower(name), cfg.Workgroup)
	return 0
}

func runSubjectsSeed(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	client := NewRegistryClient(cfg)
	result, err := client.SeedSubjects(cfg.Workgroup)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	added, _ := result["added"].([]interface{})
	if len(added) == 0 {
		fmt.Printf("Nothing to seed: every config subject in %q is already registered.\n", cfg.Workgroup)
		return 0
	}
	names := make([]string, len(added))
	for i, a := range added {
		names[i] = fmt.Sprintf("%v", a)
	}
	fmt.Printf("Seeded %d subject(s) into %s: %s\n", len(added), cfg.Workgroup, strings.Join(names, ", "))
	return 0
}

func yesNo(b bool) string {
	if b {
		return "yes"
	}
	return "no"
}

func boolVal(m map[string]interface{}, key string) bool {
	b, _ := m[key].(bool)
	return b
}
