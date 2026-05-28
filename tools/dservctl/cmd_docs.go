package main

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// cmd_docs.go - read-mostly access to db/docs.db (the dlsh / dg / ess
// command-reference database) plus an `export` helper for committing
// reviewable SQL dumps.
//
// Read paths shell out to the `sqlite3` CLI against the local docs.db
// file — no docs subprocess needed, no extra Go deps. The file is
// found via DSERV_DOCS_DB env var, /usr/local/dserv/db/docs.db, or
// ./db/docs.db (in that order).
//
// Write paths fall through to the existing `dservctl docs <tcl>` send
// mechanism, which requires a running dserv with a docs subprocess.

// runDocs implements `dservctl docs <subcommand> ...`. Unrecognized
// subcommands fall through to the existing "send Tcl to docs subprocess"
// path so that  dservctl docs "api_command_get dl_urand"  keeps working.
func runDocs(cfg *Config, args []string) int {
	if len(args) == 0 {
		printDocsUsage()
		return 0
	}
	sub := args[0]
	rest := args[1:]

	switch sub {
	case "show":
		return docsShow(cfg, rest)
	case "search":
		return docsSearch(cfg, rest)
	case "list":
		return docsList(cfg, rest)
	case "namespaces":
		return docsNamespaces(cfg, rest)
	case "export":
		return docsExport(cfg, rest)
	case "path":
		// Useful for scripts: print the resolved docs.db path
		p, err := docsDBPath()
		if err != nil {
			PrintError("%v", err)
			return 1
		}
		fmt.Println(p)
		return 0
	case "help", "--help", "-h":
		printDocsUsage()
		return 0
	}

	// Anything else: treat as raw Tcl and forward to the docs subprocess
	// (backward-compatible with the existing  dservctl docs "<tcl>"  pattern).
	cmd := strings.Join(args, " ")
	return runInterpCommand(cfg, "docs", cmd)
}

func printDocsUsage() {
	fmt.Fprintf(os.Stderr, `Usage: dservctl docs <subcommand> [args...]

Read-only commands (work against local db/docs.db, no dserv needed):
  show <slug>              Show one entry: signature, params, examples, hints
  search <query>           Full-text search across all entries
  list                     List all commands (filter with --namespace ns)
  namespaces               List namespaces with counts
  path                     Print the resolved docs.db path
  export [-o path]         Dump docs.db to SQL (default: db/docs.sql)

Authoring (sent to running dserv docs subprocess):
  <raw tcl>                Forwarded as-is, e.g.:
                             dservctl docs "docs_authoring_enable"
                             dservctl docs "docsdb::add_entry -slug ... "

Common flags:
  --json                   Machine-readable JSON output (show/search/list/namespaces)
  --namespace ns           Restrict to one namespace (list/search)
  --limit N                Cap result count (search/list)

The docs.db location is resolved in this order:
  $DSERV_DOCS_DB
  /usr/local/dserv/db/docs.db
  ./db/docs.db
`)
}

// ---------- DB path & sqlite3 invocation ----------

func docsDBPath() (string, error) {
	candidates := []string{
		os.Getenv("DSERV_DOCS_DB"), // explicit override
		"db/docs.db",               // dev authoring copy (gitignored)
		"build/db/docs.db",         // dev build artifact (CMake builds from docs.sql)
		"/usr/local/dserv/db/docs.db", // installed
	}
	for _, p := range candidates {
		if p == "" {
			continue
		}
		if _, err := os.Stat(p); err == nil {
			abs, _ := filepath.Abs(p)
			return abs, nil
		}
	}
	return "", fmt.Errorf("docs.db not found (set DSERV_DOCS_DB or run from a dir containing db/docs.db)")
}

// sqliteQueryJSON runs a SELECT and returns the result as a slice of
// row maps (sqlite3's `.mode json` output).
func sqliteQueryJSON(dbPath, sql string) ([]map[string]any, error) {
	cmd := exec.Command("sqlite3", "-readonly", dbPath, "-cmd", ".mode json", sql)
	out, err := cmd.Output()
	if err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			return nil, fmt.Errorf("sqlite3: %s", strings.TrimSpace(string(ee.Stderr)))
		}
		return nil, err
	}
	out = []byte(strings.TrimSpace(string(out)))
	if len(out) == 0 {
		return nil, nil
	}
	var rows []map[string]any
	if err := json.Unmarshal(out, &rows); err != nil {
		return nil, fmt.Errorf("parsing sqlite output: %w", err)
	}
	return rows, nil
}

// ---------- show ----------

func docsShow(cfg *Config, args []string) int {
	asJSON := cfg.JSON
	var slug string
	for _, a := range args {
		switch a {
		case "--json":
			asJSON = true
		default:
			if strings.HasPrefix(a, "--") {
				PrintError("unknown flag: %s", a)
				return 2
			}
			if slug != "" {
				PrintError("expected one slug, got: %q and %q", slug, a)
				return 2
			}
			slug = a
		}
	}
	if slug == "" {
		PrintError("usage: dservctl docs show <slug> [--json]")
		return 2
	}

	dbPath, err := docsDBPath()
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	// Resolve via name OR slug — entries' .title equals the command name
	// for command-type entries, so try both.
	esc := strings.ReplaceAll(slug, "'", "''")
	entryRows, err := sqliteQueryJSON(dbPath, fmt.Sprintf(
		`SELECT id, slug, title, summary, namespace, syntax, return_type,
		        see_also, stability, deprecated, deprecated_msg, content
		   FROM entries
		  WHERE slug='%s' OR title='%s'
		  LIMIT 1`, esc, esc))
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	if len(entryRows) == 0 {
		PrintError("not found: %s", slug)
		return 1
	}
	entry := entryRows[0]
	entryID := fmt.Sprintf("%v", entry["id"])

	paramRows, _ := sqliteQueryJSON(dbPath, fmt.Sprintf(
		`SELECT sort_order, name, param_type, is_optional, default_value, description
		   FROM parameters WHERE entry_id=%s ORDER BY sort_order`, entryID))
	exampleRows, _ := sqliteQueryJSON(dbPath, fmt.Sprintf(
		`SELECT title, description, code, expected_output, output_type, example_type
		   FROM examples WHERE entry_id=%s ORDER BY sort_order`, entryID))
	hintRows, _ := sqliteQueryJSON(dbPath, fmt.Sprintf(
		`SELECT hint_text FROM hints WHERE entry_id=%s ORDER BY sort_order`, entryID))

	if asJSON {
		out := map[string]any{
			"entry":      entry,
			"parameters": paramRows,
			"examples":   exampleRows,
			"hints":      hintRows,
		}
		b, _ := json.MarshalIndent(out, "", "  ")
		fmt.Println(string(b))
		return 0
	}

	// Human-readable rendering
	fmt.Printf("%s\n", strVal(entry, "title"))
	if dep, _ := entry["deprecated"].(float64); dep != 0 {
		fmt.Printf("  [DEPRECATED] %s\n", strVal(entry, "deprecated_msg"))
	}
	if s := strVal(entry, "stability"); s != "" && s != "stable" {
		fmt.Printf("  stability: %s\n", s)
	}
	if s := strVal(entry, "namespace"); s != "" {
		fmt.Printf("  namespace: %s\n", s)
	}
	if s := strVal(entry, "syntax"); s != "" {
		fmt.Printf("  syntax:    %s\n", s)
	}
	if s := strVal(entry, "return_type"); s != "" {
		fmt.Printf("  returns:   %s\n", s)
	}
	if s := strVal(entry, "summary"); s != "" {
		fmt.Printf("\n%s\n", s)
	}
	if s := strVal(entry, "content"); s != "" {
		fmt.Printf("\nDESCRIPTION\n%s\n", strings.TrimSpace(s))
	}
	if len(paramRows) > 0 {
		fmt.Printf("\nPARAMETERS\n")
		for _, p := range paramRows {
			opt := ""
			if v, _ := p["is_optional"].(float64); v != 0 {
				opt = " (optional)"
			}
			tp := strVal(p, "param_type")
			if tp != "" {
				tp = " [" + tp + "]"
			}
			fmt.Printf("  %s%s%s\n", strVal(p, "name"), tp, opt)
			if d := strVal(p, "description"); d != "" {
				fmt.Printf("      %s\n", d)
			}
		}
	}
	if len(exampleRows) > 0 {
		fmt.Printf("\nEXAMPLES\n")
		for _, ex := range exampleRows {
			if t := strVal(ex, "title"); t != "" {
				fmt.Printf("  — %s\n", t)
			}
			if d := strVal(ex, "description"); d != "" {
				fmt.Printf("    %s\n", d)
			}
			if c := strVal(ex, "code"); c != "" {
				for _, line := range strings.Split(strings.TrimRight(c, "\n"), "\n") {
					fmt.Printf("    %s\n", line)
				}
			}
			if o := strVal(ex, "expected_output"); o != "" {
				for _, line := range strings.Split(strings.TrimRight(o, "\n"), "\n") {
					fmt.Printf("    %s\n", line)
				}
			}
			fmt.Println()
		}
	}
	if len(hintRows) > 0 {
		fmt.Printf("HINTS\n")
		for _, h := range hintRows {
			fmt.Printf("  • %s\n", strVal(h, "hint_text"))
		}
	}
	if s := strVal(entry, "see_also"); s != "" {
		fmt.Printf("\nSEE ALSO\n  %s\n", s)
	}
	return 0
}

// ---------- search ----------

func docsSearch(cfg *Config, args []string) int {
	asJSON := cfg.JSON
	var ns string
	var query string
	limit := 20
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch a {
		case "--json":
			asJSON = true
		case "--namespace":
			if i+1 < len(args) {
				ns = args[i+1]
				i++
			}
		case "--limit":
			if i+1 < len(args) {
				fmt.Sscanf(args[i+1], "%d", &limit)
				i++
			}
		default:
			if strings.HasPrefix(a, "--") {
				PrintError("unknown flag: %s", a)
				return 2
			}
			if query == "" {
				query = a
			} else {
				query = query + " " + a
			}
		}
	}
	if query == "" {
		PrintError("usage: dservctl docs search <query> [--namespace ns] [--limit N] [--json]")
		return 2
	}
	dbPath, err := docsDBPath()
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	esc := strings.ReplaceAll(query, "'", "''")
	nsFilter := ""
	if ns != "" {
		nsFilter = fmt.Sprintf(" AND e.namespace='%s'", strings.ReplaceAll(ns, "'", "''"))
	}
	sql := fmt.Sprintf(`
		SELECT e.slug, e.title, e.namespace, e.summary,
		       snippet(entries_fts, -1, '[', ']', '...', 8) AS snip
		  FROM entries_fts
		  JOIN entries e ON e.id = entries_fts.rowid
		 WHERE entries_fts MATCH '%s'%s
		   AND e.published = 1
		 ORDER BY rank
		 LIMIT %d`, esc, nsFilter, limit)
	rows, err := sqliteQueryJSON(dbPath, sql)
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	if asJSON {
		b, _ := json.MarshalIndent(rows, "", "  ")
		fmt.Println(string(b))
		return 0
	}
	if len(rows) == 0 {
		fmt.Println("(no matches)")
		return 0
	}
	for _, r := range rows {
		fmt.Printf("%-24s %-6s %s\n", strVal(r, "title"), strVal(r, "namespace"), strVal(r, "summary"))
		if s := strVal(r, "snip"); s != "" && s != strVal(r, "summary") {
			fmt.Printf("    %s\n", s)
		}
	}
	return 0
}

// ---------- list ----------

func docsList(cfg *Config, args []string) int {
	asJSON := cfg.JSON
	var ns string
	limit := 0
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch a {
		case "--json":
			asJSON = true
		case "--namespace":
			if i+1 < len(args) {
				ns = args[i+1]
				i++
			}
		case "--limit":
			if i+1 < len(args) {
				fmt.Sscanf(args[i+1], "%d", &limit)
				i++
			}
		default:
			if strings.HasPrefix(a, "--") {
				PrintError("unknown flag: %s", a)
				return 2
			}
		}
	}
	dbPath, err := docsDBPath()
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	where := "WHERE published=1 AND entry_type='command'"
	if ns != "" {
		where += fmt.Sprintf(" AND namespace='%s'", strings.ReplaceAll(ns, "'", "''"))
	}
	lim := ""
	if limit > 0 {
		lim = fmt.Sprintf(" LIMIT %d", limit)
	}
	sql := fmt.Sprintf(`SELECT title, namespace, syntax, summary
	                      FROM entries %s ORDER BY namespace, title%s`, where, lim)
	rows, err := sqliteQueryJSON(dbPath, sql)
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	if asJSON {
		b, _ := json.MarshalIndent(rows, "", "  ")
		fmt.Println(string(b))
		return 0
	}
	for _, r := range rows {
		fmt.Printf("%-28s %-6s %s\n", strVal(r, "title"), strVal(r, "namespace"), strVal(r, "summary"))
	}
	fmt.Fprintf(os.Stderr, "(%d entries)\n", len(rows))
	return 0
}

// ---------- namespaces ----------

func docsNamespaces(cfg *Config, args []string) int {
	asJSON := cfg.JSON
	for _, a := range args {
		if a == "--json" {
			asJSON = true
		}
	}
	dbPath, err := docsDBPath()
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	rows, err := sqliteQueryJSON(dbPath, `
		SELECT namespace, COUNT(*) AS n
		  FROM entries
		 WHERE published=1 AND entry_type='command'
		 GROUP BY namespace
		 ORDER BY n DESC`)
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	if asJSON {
		b, _ := json.MarshalIndent(rows, "", "  ")
		fmt.Println(string(b))
		return 0
	}
	for _, r := range rows {
		fmt.Printf("%-8s %v\n", strVal(r, "namespace"), r["n"])
	}
	return 0
}

// ---------- export ----------

func docsExport(cfg *Config, args []string) int {
	out := "db/docs.sql"
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-o", "--out":
			if i+1 < len(args) {
				out = args[i+1]
				i++
			}
		default:
			if strings.HasPrefix(args[i], "--") {
				PrintError("unknown flag: %s", args[i])
				return 2
			}
		}
	}
	dbPath, err := docsDBPath()
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	// Prefer the repo's export script if we're in the repo (it strips
	// the FTS shadow tables); otherwise do a plain .dump here.
	script := "scripts/export-docs.sh"
	if _, err := os.Stat(script); err == nil {
		cmd := exec.Command("bash", script, out)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		return waitExit(cmd.Run())
	}
	cmd := exec.Command("sqlite3", dbPath, ".dump")
	f, err := os.Create(out)
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	defer f.Close()
	cmd.Stdout = f
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		PrintError("%v", err)
		return 1
	}
	fmt.Fprintf(os.Stderr, "wrote %s\n", out)
	return 0
}

func waitExit(err error) int {
	if err == nil {
		return 0
	}
	if ee, ok := err.(*exec.ExitError); ok {
		return ee.ExitCode()
	}
	return 1
}
