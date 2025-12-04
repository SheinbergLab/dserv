#!/usr/bin/env python3
"""
import_dokuwiki.py - Import DokuWiki documentation into docs database

Parses DokuWiki .txt files and imports them into the SQLite docs database.

DokuWiki format:
    ====== function_name ======
    ===== Description =====
    Short description
    ===== Synopsis =====
    function args...
    ===== Arguments =====
    |arg |→ description |
    ===== Returns =====
    Return description
    ===== Details =====
    Full description
    ===== Example =====
    <code>
    example code
    </code>
    ===== See Also =====
    [[other_func]], [[another_func]]

Usage:
    python import_dokuwiki.py /path/to/wiki_dir /path/to/docs.db
"""

import argparse
import re
import sqlite3
from pathlib import Path
from datetime import datetime


class DokuWikiParser:
    """Parse DokuWiki format into structured data"""
    
    # Section header pattern
    SECTION_PATTERN = re.compile(r'^=====\s*(.+?)\s*=====\s*$')
    
    # Title pattern (6 equals signs)
    TITLE_PATTERN = re.compile(r'^======\s*(.+?)\s*======\s*$')
    
    # Code block patterns
    CODE_START = re.compile(r'<code>')
    CODE_END = re.compile(r'</code>')
    
    # Wiki link pattern [[link]]
    LINK_PATTERN = re.compile(r'\[\[([^\]]+)\]\]')
    
    # Table row pattern
    TABLE_PATTERN = re.compile(r'^\|(.+)\|$')
    
    def __init__(self):
        pass
    
    def parse_file(self, filepath):
        """Parse a DokuWiki file and return structured data"""
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
        
        return self.parse_content(content, filepath.stem)
    
    def parse_content(self, content, default_name='unknown'):
        """Parse DokuWiki content string"""
        lines = content.split('\n')
        
        result = {
            'name': default_name,
            'description': '',
            'synopsis': '',
            'arguments': '',
            'returns': '',
            'details': '',
            'example': '',
            'see_also': '',
            'raw_content': content,
        }
        
        current_section = None
        section_content = []
        in_code_block = False
        
        for line in lines:
            # Check for title
            title_match = self.TITLE_PATTERN.match(line)
            if title_match:
                result['name'] = title_match.group(1).strip()
                continue
            
            # Check for section header
            section_match = self.SECTION_PATTERN.match(line)
            if section_match and not in_code_block:
                # Save previous section
                if current_section:
                    self._save_section(result, current_section, section_content)
                
                current_section = section_match.group(1).strip().lower()
                section_content = []
                continue
            
            # Track code blocks
            if self.CODE_START.search(line):
                in_code_block = True
            if self.CODE_END.search(line):
                in_code_block = False
            
            # Accumulate content
            if current_section:
                section_content.append(line)
        
        # Save last section
        if current_section:
            self._save_section(result, current_section, section_content)
        
        # Post-process
        result['description'] = self._clean_text(result['description'])
        result['synopsis'] = self._clean_text(result['synopsis'])
        result['returns'] = self._clean_text(result['returns'])
        result['details'] = self._clean_text(result['details'])
        result['arguments'] = self._parse_arguments(result['arguments'])
        result['example'] = self._extract_code(result['example'])
        result['see_also'] = self._parse_see_also(result['see_also'])
        
        return result
    
    def _save_section(self, result, section_name, content):
        """Map section name to result field"""
        text = '\n'.join(content)
        
        section_map = {
            'description': 'description',
            'synopsis': 'synopsis',
            'arguments': 'arguments',
            'returns': 'returns',
            'details': 'details',
            'example': 'example',
            'examples': 'example',
            'see also': 'see_also',
            'seealso': 'see_also',
        }
        
        field = section_map.get(section_name)
        if field:
            result[field] = text
    
    def _clean_text(self, text):
        """Clean up text - remove extra whitespace, wiki formatting"""
        if not text:
            return ''
        
        # Remove leading/trailing whitespace from each line
        lines = [line.strip() for line in text.split('\n')]
        
        # Remove empty lines at start and end
        while lines and not lines[0]:
            lines.pop(0)
        while lines and not lines[-1]:
            lines.pop()
        
        # Join and clean up
        text = '\n'.join(lines)
        
        # Convert wiki formatting to plain text or markdown
        # ''text'' -> `text` (wiki monospace to markdown)
        text = re.sub(r"''([^']+)''", r'`\1`', text)
        
        # //text// -> *text* (wiki italic to markdown)
        text = re.sub(r'//([^/]+)//', r'*\1*', text)
        
        # **text** stays the same (bold)
        
        # Remove trailing backslashes (wiki line breaks)
        text = re.sub(r'\\\\\s*$', '', text, flags=re.MULTILINE)
        
        return text.strip()
    
    def _parse_arguments(self, text):
        """Parse arguments table into structured format"""
        if not text:
            return ''
        
        lines = text.split('\n')
        args = []
        
        for line in lines:
            # Match table rows
            match = self.TABLE_PATTERN.match(line.strip())
            if match:
                cells = [c.strip() for c in match.group(1).split('|')]
                if len(cells) >= 2:
                    # First cell is arg name, rest is description
                    arg_name = cells[0].strip()
                    # Handle arrow separator (→ or ->)
                    desc_parts = []
                    for cell in cells[1:]:
                        cell = cell.strip()
                        if cell.startswith('→') or cell.startswith('->'):
                            cell = cell[1:].strip()
                        if cell:
                            desc_parts.append(cell)
                    desc = ' '.join(desc_parts)
                    
                    if arg_name and arg_name != '→' and arg_name != '->':
                        args.append(f"{arg_name}: {desc}")
        
        return '\n'.join(args)
    
    def _extract_code(self, text):
        """Extract code from <code>...</code> blocks"""
        if not text:
            return ''
        
        # Find all code blocks
        code_blocks = re.findall(r'<code>(.*?)</code>', text, re.DOTALL)
        
        if code_blocks:
            # Join multiple blocks with blank line
            return '\n\n'.join(block.strip() for block in code_blocks)
        
        # If no code tags, return cleaned text
        return self._clean_text(text)
    
    def _parse_see_also(self, text):
        """Extract function names from see also links"""
        if not text:
            return ''
        
        # Find all [[link]] patterns
        links = self.LINK_PATTERN.findall(text)
        
        # Clean up link names
        cleaned = []
        for link in links:
            # Remove any namespace prefix and clean
            name = link.split('|')[0]  # Handle [[link|text]] format
            name = name.split(':')[-1]  # Handle namespace:page format
            name = name.strip()
            if name:
                cleaned.append(name)
        
        return ', '.join(cleaned)


class DocsImporter:
    """Import parsed wiki docs into SQLite database"""
    
    def __init__(self, db_path):
        self.db_path = Path(db_path)
        self.conn = None
        self.category_cache = {}
    
    def connect(self):
        """Connect to database"""
        if not self.db_path.exists():
            raise FileNotFoundError(f"Database not found: {self.db_path}")
        self.conn = sqlite3.connect(self.db_path)
        self._load_categories()
    
    def _load_categories(self):
        """Load category name -> id mapping"""
        cursor = self.conn.cursor()
        cursor.execute('SELECT id, name FROM categories')
        for row in cursor.fetchall():
            self.category_cache[row[1]] = row[0]
    
    def _get_or_create_category(self, category_name):
        """Get category ID, creating if needed"""
        if category_name in self.category_cache:
            return self.category_cache[category_name]
        
        cursor = self.conn.cursor()
        slug = category_name.lower().replace(' ', '-')
        cursor.execute('''
            INSERT OR IGNORE INTO categories (name, slug, description)
            VALUES (?, ?, ?)
        ''', (category_name, slug, f'{category_name} functions'))
        self.conn.commit()
        
        cursor.execute('SELECT id FROM categories WHERE name = ?', (category_name,))
        row = cursor.fetchone()
        if row:
            self.category_cache[category_name] = row[0]
            return row[0]
        return None
    
    def close(self):
        """Close database connection"""
        if self.conn:
            self.conn.close()
    
    def _parse_arguments_structured(self, args_text):
        """Parse arguments text into list of (name, type, description, is_optional, default)"""
        if not args_text:
            return []
        
        result = []
        for line in args_text.strip().split('\n'):
            if ':' in line:
                name, desc = line.split(':', 1)
                name = name.strip()
                desc = desc.strip()
                
                # Check for optional indicator
                is_optional = 0
                default_value = ''
                if 'optional' in desc.lower() or 'default' in desc.lower():
                    is_optional = 1
                    # Try to extract default value
                    import re
                    default_match = re.search(r'default[:\s]+([^\s,]+)', desc, re.IGNORECASE)
                    if default_match:
                        default_value = default_match.group(1)
                
                # Guess type from description
                param_type = ''
                desc_lower = desc.lower()
                if 'integer' in desc_lower or 'int' in desc_lower:
                    param_type = 'int'
                elif 'float' in desc_lower or 'numeric' in desc_lower or 'number' in desc_lower:
                    param_type = 'numeric'
                elif 'string' in desc_lower:
                    param_type = 'string'
                elif 'list' in desc_lower:
                    param_type = 'list'
                elif 'boolean' in desc_lower or 'bool' in desc_lower:
                    param_type = 'boolean'
                
                result.append((name, param_type, desc, is_optional, default_value))
        
        return result
    
    def import_entry(self, parsed, namespace=None, source_file=None):
        """Import a parsed wiki entry into the database"""
        cursor = self.conn.cursor()
        
        name = parsed['name']
        slug = name  # Use function name as slug
        
        # Determine namespace from name if not provided
        if not namespace:
            for prefix in ['dl_', 'dg_', 'dlg_', 'dlp_', 'dm_' ]:
                if name.startswith(prefix):
                    namespace = prefix.rstrip('_')
                    break
            else:
                namespace = 'misc'
        
        # Determine category based on namespace
        category_map = {
            'dl': 'Dynamic Lists',
            'dg': 'Dyanmic Groups', 
            'dlg': 'Graphics',
            'dlp': 'Plotting',
            'dm': 'Matrices',
        }
        category_name = category_map.get(namespace, 'Other')
        category_id = self._get_or_create_category(category_name)
        
        # Check if entry exists
        cursor.execute('SELECT id FROM entries WHERE slug = ?', (slug,))
        existing = cursor.fetchone()
        
        # Content is the detailed description (not arguments/examples - those go in separate tables)
        content = parsed['details'] if parsed['details'] else parsed['description']
        
        if existing:
            entry_id = existing[0]
            # Update existing entry
            cursor.execute('''
                UPDATE entries SET
                    entry_type = 'command',
                    category_id = ?,
                    title = ?,
                    summary = ?,
                    content = ?,
                    namespace = ?,
                    syntax = ?,
                    return_type = ?,
                    see_also = ?
                WHERE slug = ?
            ''', (
                category_id,
                name,
                parsed['description'][:500] if parsed['description'] else '',
                content,
                namespace,
                parsed['synopsis'],
                parsed['returns'],
                parsed['see_also'],
                slug,
            ))
            
            # Clear old parameters and examples
            cursor.execute('DELETE FROM parameters WHERE entry_id = ?', (entry_id,))
            cursor.execute('DELETE FROM examples WHERE entry_id = ?', (entry_id,))
            
            action = 'updated'
        else:
            # Insert new entry
            cursor.execute('''
                INSERT INTO entries (
                    slug, entry_type, category_id, title, summary, content,
                    namespace, syntax, return_type, see_also
                ) VALUES (?, 'command', ?, ?, ?, ?, ?, ?, ?, ?)
            ''', (
                slug,
                category_id,
                name,
                parsed['description'][:500] if parsed['description'] else '',
                content,
                namespace,
                parsed['synopsis'],
                parsed['returns'],
                parsed['see_also'],
            ))
            entry_id = cursor.lastrowid
            action = 'inserted'
        
        # Insert parameters
        args = self._parse_arguments_structured(parsed['arguments'])
        for i, (arg_name, param_type, desc, is_optional, default_val) in enumerate(args):
            cursor.execute('''
                INSERT INTO parameters (entry_id, name, param_type, description, is_optional, default_value, sort_order)
                VALUES (?, ?, ?, ?, ?, ?, ?)
            ''', (entry_id, arg_name, param_type, desc, is_optional, default_val, i))
        
        # Insert example if present
        if parsed['example']:
            cursor.execute('''
                INSERT INTO examples (entry_id, title, code, example_type, sort_order)
                VALUES (?, 'Usage', ?, 'example', 0)
            ''', (entry_id, parsed['example']))
        
        return action, name
    
    def commit(self):
        """Commit changes"""
        self.conn.commit()


def get_namespace_from_path(filepath):
    """Extract namespace from file path"""
    # Path like: .../dl_functions/dl_foo.txt -> 'dl'
    parent = filepath.parent.name
    if parent.endswith('_functions'):
        return parent.replace('_functions', '')
    return None


def main():
    parser = argparse.ArgumentParser(description='Import DokuWiki docs into database')
    parser.add_argument('wiki_dir', help='Directory containing DokuWiki files')
    parser.add_argument('database', help='Path to SQLite database')
    parser.add_argument('--dry-run', action='store_true', help='Parse but do not import')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    args = parser.parse_args()
    
    wiki_dir = Path(args.wiki_dir)
    if not wiki_dir.exists():
        print(f"Error: Wiki directory not found: {wiki_dir}")
        return 1
    
    # Find all .txt files (excluding 0start.txt index files and AppleDouble)
    txt_files = [f for f in wiki_dir.rglob('*.txt') 
                 if f.name != '0start.txt' and '.AppleDouble' not in str(f)]
    print(f"Found {len(txt_files)} wiki files")
    
    wiki_parser = DokuWikiParser()
    
    if args.dry_run:
        # Just parse and show results
        for filepath in txt_files[:5]:  # Show first 5
            parsed = wiki_parser.parse_file(filepath)
            print(f"\n{'='*60}")
            print(f"File: {filepath}")
            print(f"Name: {parsed['name']}")
            print(f"Description: {parsed['description'][:100]}...")
            print(f"Synopsis: {parsed['synopsis']}")
            print(f"Arguments: {parsed['arguments'][:100]}..." if parsed['arguments'] else "Arguments: (none)")
            print(f"Example: {parsed['example'][:100]}..." if parsed['example'] else "Example: (none)")
        return 0
    
    # Import into database
    importer = DocsImporter(args.database)
    importer.connect()
    
    stats = {'inserted': 0, 'updated': 0, 'errors': 0}
    
    for filepath in txt_files:
        try:
            parsed = wiki_parser.parse_file(filepath)
            namespace = get_namespace_from_path(filepath)
            action, name = importer.import_entry(parsed, namespace, str(filepath))
            stats[action] += 1
            
            if args.verbose:
                print(f"{action}: {name}")
                
        except Exception as e:
            stats['errors'] += 1
            print(f"Error processing {filepath}: {e}")
    
    importer.commit()
    importer.close()
    
    print(f"\nImport complete:")
    print(f"  Inserted: {stats['inserted']}")
    print(f"  Updated:  {stats['updated']}")
    print(f"  Errors:   {stats['errors']}")
    
    return 0


if __name__ == '__main__':
    exit(main())
