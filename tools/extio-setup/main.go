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
	httpAddr := flag.String("http", "127.0.0.1:2569", "listen address (use :2569 for LAN/rig mode)")
	open := flag.Bool("open", true, "open the default browser on start")
	fwDir := flag.String("fw", "", "directory of firmware .uf2 images (default: auto-detect wiznet-io/dist)")
	dev := flag.Bool("dev", false, "serve web/ from disk instead of the embedded copy")
	flag.Parse()

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

	ln, err := net.Listen("tcp", *httpAddr)
	if err != nil {
		// Another instance already running? Just front it and exit.
		url := "http://" + hostURL(*httpAddr)
		if ping(url) {
			fmt.Printf("extio-setup already running at %s\n", url)
			if *open {
				openBrowser(url)
			}
			return
		}
		log.Fatalf("listen %s: %v", *httpAddr, err)
	}

	url := "http://" + hostURL(ln.Addr().String())
	fmt.Printf("extio-setup %s serving %s", version, url)
	if fw != "" {
		fmt.Printf("  (firmware: %s)", fw)
	}
	fmt.Println()

	if *open {
		go func() {
			time.Sleep(150 * time.Millisecond)
			openBrowser(url)
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
