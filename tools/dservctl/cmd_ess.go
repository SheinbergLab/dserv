package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
)

// rcPrefix marks the result-code header that essEval wraps around every
// evaluation. Format on the wire is: @@RC=<catch-code>@@<result>
const rcPrefix = "@@RC="

// essEval sends a Tcl command to the ESS interpreter and reports whether it
// raised a Tcl error.
//
// We do NOT rely on the error propagating back across the `send` boundary
// (whether it does is an implementation detail of dserv's inter-interp send).
// Instead we wrap the command in `catch` *inside* the ess interp and ship the
// result code back in a sentinel header, so success/failure is unambiguous no
// matter how send behaves. `string cat` is used to assemble the reply so a
// result containing [...] or $ cannot be re-evaluated.
//
// Returns the cleaned result string and a bool that is true when the command
// errored (either an inner Tcl error or an outer transport/send error).
func essEval(cfg *Config, tcl string) (string, bool, error) {
	wrapper := fmt.Sprintf(
		"set ::__de_rc [catch {%s} ::__de_res]; return [string cat {%s} $::__de_rc {@@} $::__de_res]",
		tcl, rcPrefix)

	resp, err := Send(cfg.Host, "ess", wrapper)
	if err != nil {
		return "", true, err
	}

	// Outer error: `send ess {...}` itself failed (e.g. ess interp absent),
	// surfaced via the !TCL_ERROR prefix convention.
	output, isErr := ProcessResponse(resp)
	if isErr {
		return output, true, nil
	}

	// Inner result: parse the @@RC=<code>@@<result> header.
	if strings.HasPrefix(output, rcPrefix) {
		rest := output[len(rcPrefix):]
		if idx := strings.Index(rest, "@@"); idx >= 0 {
			code, _ := strconv.Atoi(strings.TrimSpace(rest[:idx]))
			return rest[idx+2:], code != 0, nil
		}
	}

	// Header missing (shouldn't happen) — be lenient and treat as success.
	return output, false, nil
}

// essRun is the standard "evaluate, print, set exit code" wrapper used by the
// top-level ess verbs. Errors go to stderr and yield a nonzero exit so the
// commands chain cleanly (e.g. `dservctl load X && dservctl run`).
func essRun(cfg *Config, tcl string) int {
	out, isErr, err := essEval(cfg, tcl)
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	out = strings.TrimRight(out, "\r\n")
	if isErr {
		PrintError("%s", out)
		return 1
	}
	if strings.TrimSpace(out) != "" {
		fmt.Println(out)
	}
	return 0
}

// runLoad: dservctl load <system> [protocol] [variant]
func runLoad(cfg *Config, args []string) int {
	if len(args) < 1 || len(args) > 3 {
		fmt.Fprintln(os.Stderr, "Usage: dservctl load <system> [protocol] [variant]")
		return 2
	}
	rc := essRun(cfg, "ess::load_system "+strings.Join(args, " "))
	if rc == 0 {
		fmt.Fprintf(os.Stderr, "loaded %s\n", strings.Join(args, " / "))
	}
	return rc
}

// runStimdg: dservctl stimdg [dgname]   (defaults to stimdg) -> JSON
//
// Uses the public ess::get_dg_json proc (same path the dev environment uses),
// which publishes the JSON to the ess/dev_dg_data datapoint and returns "ok",
// or an "error: ..." string if the dg is missing. We read the JSON back via
// the normal datapoint get rather than duplicating dg_toHybridJSON internals.
func runStimdg(cfg *Config, args []string) int {
	dg := "stimdg"
	if len(args) >= 1 {
		dg = args[0]
	}
	out, isErr, err := essEval(cfg, "ess::get_dg_json "+dg)
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	out = strings.TrimSpace(out)
	if isErr || out != "ok" {
		// isErr: a real Tcl error; out!="ok": the proc's "error: ..." string.
		msg := strings.TrimPrefix(out, "error: ")
		PrintError("%s", msg)
		return 1
	}
	json := dservGetClean(cfg, "ess/dev_dg_data")
	if json == "" {
		PrintError("no data published for dg %q", dg)
		return 1
	}
	fmt.Println(json)
	return 0
}

// dservGetClean fetches a datapoint from the dserv main interpreter and returns
// its trimmed value, or "" on error.
func dservGetClean(cfg *Config, dp string) string {
	resp, err := SendToDserv(cfg.Host, "dservGet "+dp)
	if err != nil {
		return ""
	}
	out, isErr := ProcessResponse(resp)
	if isErr {
		return ""
	}
	return strings.TrimSpace(out)
}
