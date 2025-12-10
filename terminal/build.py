#!/usr/bin/env python3
"""
Build script to combine HTML, CSS, and JS into a single embeddable file.

Usage: python build.py
Output: console.html (single file ready for dserv embedding)
        Also copies to ../www/terminal.html
"""

from pathlib import Path

def read_file(filepath):
    """Read a file and return its contents."""
    with open(filepath, 'r', encoding='utf-8') as f:
        return f.read()

def build():
    """Build the combined HTML file."""
    base_dir = Path(__file__).parent
    
    print("Building console.html...")
    
    # Read source files
    html = read_file(base_dir / 'index.html')
    css = read_file(base_dir / 'styles.css')
    app_js = read_file(base_dir / 'app.js')
    terminal_js = read_file(base_dir / 'terminal.js')
    datapoints_js = read_file(base_dir / 'datapoints.js')
    errors_js = read_file(base_dir / 'errors.js')
    
    # Replace external CSS with inline style (simple string replace)
    html = html.replace(
        '<link rel="stylesheet" href="styles.css">',
        f'<style>\n{css}\n    </style>'
    )
    
    # Replace external JS with inline scripts (simple string replace)
    html = html.replace(
        '<script src="app.js"></script>',
        f'<script>\n{app_js}\n    </script>'
    )
    
    html = html.replace(
        '<script src="terminal.js"></script>',
        f'<script>\n{terminal_js}\n    </script>'
    )
    
    html = html.replace(
        '<script src="datapoints.js"></script>',
        f'<script>\n{datapoints_js}\n    </script>'
    )
    
    html = html.replace(
        '<script src="errors.js"></script>',
        f'<script>\n{errors_js}\n    </script>'
    )
    
    # Write output
    output_path = base_dir / 'console.html'
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(html)
    
    print(f"✓ Built {output_path}")
    print(f"  Size: {len(html):,} bytes")
    
    # Copy to ../www/terminal.html for dserv
    www_path = base_dir.parent / 'www' / 'terminal.html'
    try:
        www_path.parent.mkdir(parents=True, exist_ok=True)
        with open(www_path, 'w', encoding='utf-8') as f:
            f.write(html)
        print(f"✓ Copied to {www_path}")
    except Exception as e:
        print(f"⚠ Could not copy to {www_path}: {e}")
        print("  (This is okay if www/ directory doesn't exist yet)")
    
    print()
    print("Ready to use! Reload browser to see changes.")

if __name__ == '__main__':
    build()
