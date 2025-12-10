#!/usr/bin/env python3
"""
Documentation Development Server

Watches web/ directory for changes, runs build_docs.py automatically,
and serves the built files with WebSocket proxy to dserv.

Usage:
    python dev_server.py [options]
    
    # Or with uv:
    uv run dev_server.py [options]

Options:
    --port PORT        HTTP server port (default: 8000)
    --dserv HOST:PORT  dserv WebSocket endpoint (default: localhost:2570)
    --web-dir DIR      Web source directory (default: ./web)
    --build-script     Path to build_docs.py (default: ./build_docs.py)
    --no-watch         Disable file watching (just serve)
"""

import argparse
import http.server
import json
import os
import socketserver
import subprocess
import sys
import threading
import time
from pathlib import Path
from urllib.parse import urlparse

# Optional: watchdog for file system monitoring
try:
    from watchdog.observers import Observer
    from watchdog.events import FileSystemEventHandler
    HAS_WATCHDOG = True
except ImportError:
    HAS_WATCHDOG = False
    print("Note: Install 'watchdog' for auto-rebuild on file changes")
    print("      pip install watchdog  (or: uv pip install watchdog)")


class Colors:
    """ANSI color codes for terminal output"""
    RESET = "\033[0m"
    RED = "\033[91m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    BLUE = "\033[94m"
    CYAN = "\033[96m"
    GRAY = "\033[90m"


def log(msg, color=None):
    """Print a timestamped log message"""
    timestamp = time.strftime("%H:%M:%S")
    if color:
        print(f"{Colors.GRAY}[{timestamp}]{Colors.RESET} {color}{msg}{Colors.RESET}")
    else:
        print(f"{Colors.GRAY}[{timestamp}]{Colors.RESET} {msg}")


class BuildHandler(FileSystemEventHandler if HAS_WATCHDOG else object):
    """Handles file system events and triggers rebuilds"""
    
    def __init__(self, build_script, web_dir, debounce_seconds=0.5):
        self.build_script = build_script
        self.web_dir = web_dir
        self.debounce_seconds = debounce_seconds
        self.last_build = 0
        self.pending_build = False
        self.lock = threading.Lock()
        
    def on_modified(self, event):
        self._handle_event(event)
        
    def on_created(self, event):
        self._handle_event(event)
        
    def on_deleted(self, event):
        self._handle_event(event)
    
    def _handle_event(self, event):
        if event.is_directory:
            return
            
        # Only watch relevant file types
        path = Path(event.src_path)
        if path.suffix.lower() not in {'.html', '.css', '.js', '.json'}:
            return
            
        # Ignore build outputs
        if 'build' in path.parts or 'dist' in path.parts:
            return
            
        self._trigger_build(path)
    
    def _trigger_build(self, changed_path):
        """Trigger a debounced build"""
        with self.lock:
            now = time.time()
            if now - self.last_build < self.debounce_seconds:
                self.pending_build = True
                return
            self.last_build = now
            
        log(f"Changed: {changed_path.name}", Colors.YELLOW)
        self._run_build()
        
    def _run_build(self):
        """Run the build script"""
        log("Building...", Colors.CYAN)
        try:
            result = subprocess.run(
                [sys.executable, self.build_script],
                capture_output=True,
                text=True,
                cwd=self.web_dir.parent
            )
            if result.returncode == 0:
                log("Build complete ✓", Colors.GREEN)
            else:
                log(f"Build failed: {result.stderr}", Colors.RED)
        except Exception as e:
            log(f"Build error: {e}", Colors.RED)
    
    def initial_build(self):
        """Run initial build on startup"""
        self._run_build()


class DevHTTPHandler(http.server.SimpleHTTPRequestHandler):
    """HTTP handler with WebSocket proxy info and CORS headers"""
    
    def __init__(self, *args, dserv_endpoint=None, **kwargs):
        self.dserv_endpoint = dserv_endpoint
        try:
            super().__init__(*args, **kwargs)
        except Exception as e:
            # Log but don't crash on errors
            log(f"Request error: {e}", Colors.RED)
    
    def end_headers(self):
        # Add CORS headers for development
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', '*')
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate')
        super().end_headers()
    
    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()
    
    def do_GET(self):
        # Ignore favicon requests
        if self.path == '/favicon.ico':
            self.send_response(204)
            self.end_headers()
            return
            
        # Serve WebSocket endpoint info
        if self.path == '/__dserv__':
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({
                'websocket': f'ws://{self.dserv_endpoint}',
                'note': 'Connect to this WebSocket for dserv communication'
            }).encode())
            return
            
        # Serve index for root
        if self.path == '/':
            self.path = '/index.html'
            
        try:
            super().do_GET()
        except Exception as e:
            log(f"Error serving {self.path}: {e}", Colors.RED)
            self.send_error(500, str(e))
    
    def log_message(self, format, *args):
        # Custom log format
        if not args:
            return
        try:
            path = args[0].split()[1] if args else '?'
        except:
            return
        # Skip favicon logging
        if path == '/favicon.ico':
            return
        status = args[1] if len(args) > 1 else '?'
        if status == '200':
            color = Colors.GREEN
        elif status == '304':
            color = Colors.GRAY
        elif str(status).startswith('4'):
            color = Colors.YELLOW
        else:
            color = Colors.RED
        log(f"{color}{status}{Colors.RESET} {path}")


def create_handler(dserv_endpoint, directory):
    """Create a handler class with the given configuration"""
    class ConfiguredHandler(DevHTTPHandler):
        def __init__(self, *args, **kwargs):
            kwargs['directory'] = directory
            super().__init__(*args, dserv_endpoint=dserv_endpoint, **kwargs)
    return ConfiguredHandler


def create_index_html(build_dir):
    """Create a simple index page listing available docs"""
    index_path = build_dir / 'index.html'
    if index_path.exists():
        return
        
    html_files = list(build_dir.glob('*.html'))
    links = '\n'.join(
        f'        <li><a href="{f.name}">{f.stem}</a></li>'
        for f in sorted(html_files) if f.name != 'index.html'
    )
    
    index_content = f"""<!DOCTYPE html>
<html>
<head>
    <title>Documentation Dev Server</title>
    <style>
        body {{ font-family: system-ui, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; }}
        h1 {{ color: #333; }}
        ul {{ line-height: 2; }}
        a {{ color: #0066cc; }}
        .info {{ background: #f0f0f0; padding: 15px; border-radius: 5px; margin-top: 30px; }}
        code {{ background: #e0e0e0; padding: 2px 6px; border-radius: 3px; }}
    </style>
</head>
<body>
    <h1>📚 Documentation Dev Server</h1>
    <h2>Available Pages</h2>
    <ul>
{links}
    </ul>
    <div class="info">
        <strong>WebSocket:</strong> Connect to <code>ws://localhost:2570</code> for dserv<br>
        <strong>API Endpoint:</strong> <code>/__dserv__</code> returns connection info
    </div>
</body>
</html>
"""
    index_path.write_text(index_content)


def main():
    parser = argparse.ArgumentParser(description='Documentation Development Server')
    parser.add_argument('--port', type=int, default=8000, help='HTTP server port')
    parser.add_argument('--dserv', default='localhost:2570', help='dserv WebSocket host:port')
    parser.add_argument('--web-dir', default='./web', help='Web source directory')
    parser.add_argument('--build-dir', default='./build', help='Build output directory')
    parser.add_argument('--build-script', default='./build_docs.py', help='Build script path')
    parser.add_argument('--no-watch', action='store_true', help='Disable file watching')
    args = parser.parse_args()
    
    web_dir = Path(args.web_dir).resolve()
    build_dir = Path(args.build_dir).resolve()
    build_script = Path(args.build_script).resolve()
    
    # Validate paths
    if not web_dir.exists():
        print(f"Error: Web directory not found: {web_dir}")
        sys.exit(1)
    if not build_script.exists():
        print(f"Error: Build script not found: {build_script}")
        sys.exit(1)
        
    # Create build directory if needed
    build_dir.mkdir(parents=True, exist_ok=True)
    
    # Set up file watcher
    build_handler = BuildHandler(build_script, web_dir)
    observer = None
    
    if not args.no_watch:
        if HAS_WATCHDOG:
            observer = Observer()
            observer.schedule(build_handler, str(web_dir), recursive=True)
            observer.start()
            log(f"Watching {web_dir} for changes", Colors.CYAN)
        else:
            log("File watching disabled (install watchdog to enable)", Colors.YELLOW)
    
    # Run initial build
    build_handler.initial_build()
    
    # Create index page
    create_index_html(build_dir)
    
    # Start HTTP server
    handler = create_handler(args.dserv, str(build_dir))
    
    with socketserver.TCPServer(("", args.port), handler) as httpd:
        log(f"Serving at http://localhost:{args.port}", Colors.GREEN)
        log(f"dserv WebSocket: ws://{args.dserv}", Colors.BLUE)
        log("Press Ctrl+C to stop", Colors.GRAY)
        print()
        
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print()
            log("Shutting down...", Colors.YELLOW)
            if observer:
                observer.stop()
                observer.join()


if __name__ == '__main__':
    main()
