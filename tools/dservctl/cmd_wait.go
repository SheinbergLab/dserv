package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// runWait blocks until <datapoint> reaches <value>, driven by dserv's push
// channel (the same %reg/%match mechanism as `listen`) rather than polling.
// It returns the instant the value arrives. Exit codes: 0 match, 1 timeout,
// 2 usage, 130 interrupted.
//
//	dservctl wait ess/run_state complete --timeout 120
func runWait(cfg *Config, args []string) int {
	timeout := 0.0 // 0 = wait indefinitely
	var pos []string
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--timeout":
			if i+1 >= len(args) {
				fmt.Fprintln(os.Stderr, "Error: --timeout requires a value (seconds)")
				return 2
			}
			i++
			v, err := strconv.ParseFloat(args[i], 64)
			if err != nil || v <= 0 {
				fmt.Fprintf(os.Stderr, "Error: invalid --timeout value %q\n", args[i])
				return 2
			}
			timeout = v
		default:
			pos = append(pos, args[i])
		}
	}
	if len(pos) != 2 {
		fmt.Fprintln(os.Stderr, "Usage: dservctl wait <datapoint> <value> [--timeout SECONDS]")
		return 2
	}
	datapoint, target := pos[0], strings.TrimSpace(pos[1])

	// Subscribe to the push channel BEFORE the initial read, so no update is
	// missed in the window between checking the current value and subscribing.
	listener, err := net.Listen("tcp4", ":0")
	if err != nil {
		PrintError("failed to start listener: %v", err)
		return 1
	}
	defer listener.Close()
	_, listenPort, _ := net.SplitHostPort(listener.Addr().String())

	localIP, err := GetLocalIP(cfg.Host)
	if err != nil {
		PrintError("cannot reach dserv text port on %s:%d: %v", cfg.Host, DservTextPort, err)
		return 1
	}

	if _, err := SendText(cfg.Host, DservTextPort,
		fmt.Sprintf("%%reg %s %s %d", localIP, listenPort, sendJSON)); err != nil {
		PrintError("failed to register: %v", err)
		return 1
	}
	if _, err := SendText(cfg.Host, DservTextPort,
		fmt.Sprintf("%%match %s %s %s 1", localIP, listenPort, datapoint)); err != nil {
		PrintError("failed to subscribe to %q: %v", datapoint, err)
		return 1
	}

	matchCh := make(chan struct{}, 1)
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				r := bufio.NewReader(c)
				for {
					line, err := r.ReadBytes('\n')
					if err != nil {
						return
					}
					var dp Datapoint
					if json.Unmarshal([]byte(strings.TrimSpace(string(line))), &dp) != nil {
						continue
					}
					if dp.Name == datapoint &&
						strings.TrimSpace(decodeData(dp.Dtype, dp.Data)) == target {
						select {
						case matchCh <- struct{}{}:
						default:
						}
						return
					}
				}
			}(conn)
		}
	}()

	// Short-circuit if the datapoint is already at the target value.
	if dservGetClean(cfg, datapoint) == target {
		return 0
	}

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	var timeoutCh <-chan time.Time // nil channel blocks forever when timeout==0
	if timeout > 0 {
		timeoutCh = time.After(time.Duration(timeout * float64(time.Second)))
	}

	select {
	case <-matchCh:
		return 0
	case <-timeoutCh:
		PrintError("timed out after %gs waiting for %s == %q (now %q)",
			timeout, datapoint, target, dservGetClean(cfg, datapoint))
		return 1
	case <-sigCh:
		fmt.Fprintln(os.Stderr, "interrupted")
		return 130
	}
}
