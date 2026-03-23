package main

import (
	"fmt"
	"os"
)

// runBackup handles the "backup" command with subcommands: list, create, get.
func runBackup(cfg *Config, args []string) int {
	if len(args) == 0 {
		// Default to list
		return runBackupList(cfg, args)
	}

	switch args[0] {
	case "list":
		return runBackupList(cfg, args[1:])
	case "create":
		return runBackupCreate(cfg, args[1:])
	case "get":
		return runBackupGet(cfg, args[1:])
	default:
		fmt.Fprintf(os.Stderr, "Usage: dservctl backup [list|create|get]\n")
		fmt.Fprintf(os.Stderr, "\nSubcommands:\n")
		fmt.Fprintf(os.Stderr, "  list     List available backups (default)\n")
		fmt.Fprintf(os.Stderr, "  create   Create a new backup now\n")
		fmt.Fprintf(os.Stderr, "  get      Download a backup file\n")
		return 2
	}
}

func runBackupList(cfg *Config, args []string) int {
	client := NewRegistryClient(cfg)
	result, err := client.ListBackups()
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	backups := extractList(result, "backups")
	if len(backups) == 0 {
		fmt.Println("No backups found.")
		return 0
	}

	headers := []string{"FILENAME", "SIZE", "CREATED"}
	var rows [][]string
	for _, b := range backups {
		size := int64(0)
		if v, ok := b["size"].(float64); ok {
			size = int64(v)
		}
		rows = append(rows, []string{
			strVal(b, "filename"),
			formatSize(size),
			formatTime(strVal(b, "createdAt")),
		})
	}

	PrintTable(headers, rows)

	if count, ok := result["count"].(float64); ok {
		fmt.Printf("\n%d backup(s) total\n", int(count))
	}
	return 0
}

func runBackupCreate(cfg *Config, args []string) int {
	client := NewRegistryClient(cfg)
	result, err := client.CreateBackup()
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	backup, ok := result["backup"].(map[string]interface{})
	if !ok {
		fmt.Println("Backup created.")
		return 0
	}

	size := int64(0)
	if v, ok := backup["size"].(float64); ok {
		size = int64(v)
	}
	fmt.Printf("Backup created: %s (%s)\n", strVal(backup, "filename"), formatSize(size))
	return 0
}

func runBackupGet(cfg *Config, args []string) int {
	filename := ""
	output := ""

	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-o", "--output":
			if i+1 < len(args) {
				output = args[i+1]
				i++
			}
		default:
			if filename == "" {
				filename = args[i]
			}
		}
	}

	// If no filename given, fetch the latest backup
	if filename == "" {
		client := NewRegistryClient(cfg)
		result, err := client.ListBackups()
		if err != nil {
			PrintError("%v", err)
			return 1
		}
		backups := extractList(result, "backups")
		if len(backups) == 0 {
			PrintError("no backups available")
			return 1
		}
		filename = strVal(backups[0], "filename")
		fmt.Fprintf(os.Stderr, "Downloading latest backup: %s\n", filename)
	}

	if output == "" {
		output = filename
	}

	client := NewRegistryClient(cfg)
	data, err := client.GetBackup(filename)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if err := os.WriteFile(output, data, 0644); err != nil {
		PrintError("writing file: %v", err)
		return 1
	}

	fmt.Printf("Downloaded %s (%s)\n", output, formatSize(int64(len(data))))
	return 0
}

// formatSize returns a human-readable size string.
func formatSize(bytes int64) string {
	switch {
	case bytes >= 1<<30:
		return fmt.Sprintf("%.1f GB", float64(bytes)/float64(1<<30))
	case bytes >= 1<<20:
		return fmt.Sprintf("%.1f MB", float64(bytes)/float64(1<<20))
	case bytes >= 1<<10:
		return fmt.Sprintf("%.1f KB", float64(bytes)/float64(1<<10))
	default:
		return fmt.Sprintf("%d B", bytes)
	}
}
