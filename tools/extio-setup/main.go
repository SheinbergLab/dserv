// extio-setup -- bench/rig tool for extio boxes: flash, configure, monitor.
//
// A small local web server with two jobs: bridge the browser to a box's
// USB-CDC console (serial driver), and later to a dserv instance (dserv
// driver). The UI is embedded; the same assets can also be served from
// dserv's www/ where the serial features hide themselves.
//
//	extio-setup                 # localhost, opens your browser
//	extio-setup -http :2569     # LAN mode (rig helper / service)
//	extio-setup -open=false     # don't launch a browser
package main

import (
	"embed"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"runtime"
	"syscall"
	"time"
)

//go:embed web
var webFS embed.FS

var version = "dev" // stamped by -ldflags at release build

func main() {
	httpAddr := flag.String("http", "127.0.0.1:2569", "listen address; 0.0.0.0:2569 (or :2569) to accept LAN connections")
	open := flag.Bool("open", true, "open the default browser on start")
	fwDir := flag.String("fw", "", "directory of firmware .uf2 images (default: auto-detect wiznet-io/dist)")
	dev := flag.Bool("dev", false, "serve web/ from disk instead of the embedded copy")
	flag.Parse()
	// Catch the classic Go-flag trap: "-open false" makes `false` a positional
	// argument, which STOPS flag parsing -- so later flags (e.g. -http) are
	// silently dropped. Booleans need '=' (-open=false).
	if extra := flag.Args(); len(extra) > 0 {
		fmt.Fprintf(os.Stderr, "warning: ignoring extra argument(s) %v; a boolean flag needs "+
			"'=' (use -open=false, not -open false -- the latter stops flag parsing)\n", extra)
	}
	// Accept a bare host with no port (e.g. -http 0.0.0.0) by adding the default.
	addr := *httpAddr
	if _, _, err := net.SplitHostPort(addr); err != nil {
		addr = net.JoinHostPort(addr, "2569")
	}

	fw := findFirmwareDir(*fwDir)

	srv := newServer(fw)
	mux := http.NewServeMux()
	srv.routes(mux)

	// Close serial ports on the way out: a process killed mid-read can
	// leave macOS's USB-CDC driver claiming the port (EBUSY with no holder,
	// cleared only by re-enumeration). Seen live 2026-07-11.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sig
		srv.shutdown()
		os.Exit(0)
	}()

	var web fs.FS
	if *dev {
		web = os.DirFS("web")
	} else {
		var err error
		if web, err = fs.Sub(webFS, "web"); err != nil {
			log.Fatal(err)
		}
	}
	files := http.FileServerFS(web)
	if *dev { // no caching while iterating on web/
		files = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Cache-Control", "no-store")
			http.FileServerFS(web).ServeHTTP(w, r)
		})
	}
	mux.Handle("/", files)

	ln, err := net.Listen("tcp", addr)
	if err != nil {
		// Another instance already running? Just front it and exit.
		url := "http://" + hostURL(addr)
		if ping(url) {
			fmt.Printf("extio-setup already running at %s\n", url)
			if *open {
				openBrowser(url)
			}
			return
		}
		log.Fatalf("listen %s: %v", addr, err)
	}

	// The browser-open URL is always this machine's loopback; the SHOWN url is
	// LAN-reachable when bound to all interfaces -- hostURL would rewrite
	// 0.0.0.0 to 127.0.0.1, which misleadingly reads as localhost-only even
	// though the tool is reachable across the network.
	localURL := "http://" + hostURL(ln.Addr().String())
	shownURL := localURL
	if host, port, _ := net.SplitHostPort(ln.Addr().String()); host == "0.0.0.0" || host == "::" || host == "" {
		if ip := lanIP(); ip != "" {
			shownURL = "http://" + net.JoinHostPort(ip, port) + " (all interfaces)"
		} else {
			shownURL = "http://<this-host>:" + port + " (all interfaces)"
		}
	}
	fmt.Printf("extio-setup %s serving %s", version, shownURL)
	if fw != "" {
		fmt.Printf("  (firmware: %s)", fw)
	}
	fmt.Println()

	if *open {
		go func() {
			time.Sleep(150 * time.Millisecond)
			openBrowser(localURL)
		}()
	}
	log.Fatal(http.Serve(ln, mux))
}

// hostURL turns a listen address into something a browser can open.
func hostURL(addr string) string {
	host, port, err := net.SplitHostPort(addr)
	if err != nil {
		return addr
	}
	if host == "" || host == "0.0.0.0" || host == "::" {
		host = "127.0.0.1"
	}
	return net.JoinHostPort(host, port)
}

// lanIP returns this host's primary outbound IP -- the source address of the
// default route. No packet is sent; the UDP socket only resolves the route.
// "" if there is no usable route (then the caller falls back to the hostname).
func lanIP() string {
	c, err := net.Dial("udp", "8.8.8.8:80")
	if err != nil {
		return ""
	}
	defer c.Close()
	host, _, _ := net.SplitHostPort(c.LocalAddr().String())
	return host
}

func ping(url string) bool {
	c := http.Client{Timeout: 300 * time.Millisecond}
	resp, err := c.Get(url + "/api/status")
	if err != nil {
		return false
	}
	resp.Body.Close()
	return resp.StatusCode == 200
}

func openBrowser(url string) {
	switch runtime.GOOS {
	case "darwin":
		exec.Command("open", url).Start()
	case "linux":
		exec.Command("xdg-open", url).Start()
	case "windows":
		exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start()
	}
}

// findFirmwareDir locates .uf2 images: explicit flag, then the monorepo's
// wiznet-io/dist relative to cwd or the executable, then the installed path.
func findFirmwareDir(explicit string) string {
	if explicit != "" {
		return explicit
	}
	var candidates []string
	if cwd, err := os.Getwd(); err == nil {
		for _, rel := range []string{"wiznet-io/dist", "../wiznet-io/dist", "../../wiznet-io/dist"} {
			candidates = append(candidates, filepath.Join(cwd, rel))
		}
	}
	if exe, err := os.Executable(); err == nil {
		candidates = append(candidates, filepath.Join(filepath.Dir(exe), "../wiznet-io/dist"))
	}
	candidates = append(candidates, "/usr/local/dserv/firmware/extio")
	for _, c := range candidates {
		if m, _ := filepath.Glob(filepath.Join(c, "*.uf2")); len(m) > 0 {
			return c
		}
	}
	return ""
}
