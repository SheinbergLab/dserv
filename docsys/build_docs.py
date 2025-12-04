#!/usr/bin/env python3
"""
build_docs.py - Build documentation HTML files for CMake embedding

Inlines CSS from <link rel="stylesheet" href="..."> 
Inlines JS from <script src="..."></script>

Directory structure:
    web/
    ├── html/           HTML source files
    ├── js/             JavaScript libraries
    └── css/            Stylesheets

Usage:
    python build_docs.py --source web --output build
"""

import argparse
import re
from pathlib import Path
from datetime import datetime

class DocBuilder:
    def __init__(self, source_dir, output_dir):
        self.source_dir = Path(source_dir)
        self.output_dir = Path(output_dir)
        
        # Source subdirectories
        self.html_dir = self.source_dir / 'html'
        self.js_dir = self.source_dir / 'js'
        self.css_dir = self.source_dir / 'css'
        
        # Verify directories exist
        if not self.html_dir.exists():
            raise FileNotFoundError(f"HTML directory not found: {self.html_dir}")
        if not self.js_dir.exists():
            raise FileNotFoundError(f"JavaScript directory not found: {self.js_dir}")
        if not self.css_dir.exists():
            raise FileNotFoundError(f"CSS directory not found: {self.css_dir}")
        
        # Create output directory if needed
        self.output_dir.mkdir(parents=True, exist_ok=True)
    
    def read_file(self, directory, filename):
        """Read a file from the specified directory"""
        filepath = directory / filename
        if not filepath.exists():
            raise FileNotFoundError(f"File not found: {filepath}")
        return filepath.read_text(encoding='utf-8')
    
    def inline_css(self, html_content):
        """Replace <link rel="stylesheet" href="X.css"> with inline <style> tags"""
        
        def replace_link(match):
            full_tag = match.group(0)
            
            # Extract href
            href_match = re.search(r'href=["\']([^"\']+\.css)["\']', full_tag)
            if not href_match:
                return full_tag
            
            href = href_match.group(1)
            
            # Skip external URLs
            if href.startswith('http://') or href.startswith('https://'):
                return full_tag
            
            # Read and inline CSS
            try:
                css_content = self.read_file(self.css_dir, href)
                return f'<style>\n{css_content}\n</style>'
            except FileNotFoundError:
                print(f"  Warning: CSS file not found: {href}")
                return full_tag
        
        # Only match <link> tags with .css files
        pattern = r'<link[^>]+href=["\'][^"\']+\.css["\'][^>]*>'
        return re.sub(pattern, replace_link, html_content)
    
    def inline_javascript(self, html_content):
        """Replace <script src="X.js"></script> with inline <script> tags"""
        
        def replace_script(match):
            full_tag = match.group(0)
            
            # Extract src
            src_match = re.search(r'src=["\']([^"\']+\.js)["\']', full_tag)
            if not src_match:
                return full_tag
            
            src = src_match.group(1)
            
            # Skip external URLs
            if src.startswith('http://') or src.startswith('https://'):
                return full_tag
            
            # Read and inline JS
            try:
                js_content = self.read_file(self.js_dir, src)
                return f'<script>\n{js_content}\n</script>'
            except FileNotFoundError:
                print(f"  Warning: JS file not found: {src}")
                return full_tag
        
        # Only match <script> tags with .js files
        pattern = r'<script[^>]+src=["\'][^"\']+\.js["\'][^>]*>\s*</script>'
        return re.sub(pattern, replace_script, html_content)
    
    def add_build_marker(self, html_content):
        """Add a comment indicating when the file was built"""
        marker = f"<!-- Built: {datetime.now().isoformat()} by build_docs.py -->\n"
        if html_content.startswith('<!DOCTYPE'):
            newline_pos = html_content.find('\n')
            if newline_pos != -1:
                return html_content[:newline_pos+1] + marker + html_content[newline_pos+1:]
        return marker + html_content
    
    def build_file(self, input_filename, output_filename):
        """Build a single HTML file"""
        print(f"  Building {output_filename}...")
        
        # Read source HTML
        html_content = self.read_file(self.html_dir, input_filename)
        
        # Inline CSS first
        html_content = self.inline_css(html_content)
        
        # Then inline JavaScript
        html_content = self.inline_javascript(html_content)
        
        # Add build marker
        html_content = self.add_build_marker(html_content)
        
        # Write output
        output_path = self.output_dir / output_filename
        output_path.write_text(html_content, encoding='utf-8')
        
        # Report size
        size_kb = output_path.stat().st_size / 1024
        print(f"    Created: {output_path} ({size_kb:.1f} KB)")
    
    def build_all(self):
        """Build all documentation files"""
        print("Building documentation HTML files...")
        print(f"Source: {self.source_dir}")
        print(f"Output: {self.output_dir}")
        print()
        
        files = [
            ('index.html', 'docs_index.html'),
            ('command_reference.html', 'docs_reference.html'),
            ('content_editor.html', 'docs_content_editor.html'),
            ('command_editor.html', 'docs_command_editor.html'),
            ('tutorial.html', 'docs_tutorial.html'),
            ('DGTableViewer.html', 'DGTableViewer.html'),
            ('TclTerminal.html', 'TclTerminal.html'),
            ('GraphicsDemo.html', 'GraphicsDemo.html'),
        ]
        
        built_count = 0
        for input_file, output_file in files:
            try:
                self.build_file(input_file, output_file)
                built_count += 1
            except FileNotFoundError:
                print(f"  Skipping {output_file}: source not found")
        
        print()
        print(f"Successfully built {built_count} file(s)")

def main():
    parser = argparse.ArgumentParser(description='Build documentation HTML files')
    parser.add_argument('--source', default='web', help='Source directory (default: web)')
    parser.add_argument('--output', default='build', help='Output directory (default: build)')
    args = parser.parse_args()
    
    try:
        builder = DocBuilder(args.source, args.output)
        builder.build_all()
    except Exception as e:
        print(f"Error: {e}")
        return 1
    
    return 0

if __name__ == '__main__':
    exit(main())
