# =============================================================================
# Documentation Subprocess Configuration (docsconf.tcl)
# =============================================================================
# Source this file in your "docs" subprocess to enable the documentation API.
#
# In dserv startup:
#   subprocess create docs docsconf.tcl
#
# Web clients can then call:
#   send docs {api_entries_list {type tutorial}}
#   send docs {api_entry_get some-slug}
#   send docs {api_search {query "dl_fromto"}}
#
# =============================================================================

# enable error logging
errormon enable

# Configuration - adjust path as needed
tcl::tm::add $::dspath/lib
set ::docs_db_path "/usr/local/dserv/db/docs.db"

# Check for authoring mode (set in /usr/local/dserv/local/docs.tcl if needed)
if {![info exists ::docs_authoring_mode]} {
    set ::docs_authoring_mode 0
}

# Load modules (assumes docsdb-1.0.tm and docsapi-1.0.tm are in auto_path)
package require docsdb
package require docsapi

package require dlsh

proc on_shutdown {} {
    puts "Docs subprocess shutting down..."
    docsdb::close
    puts "Docs subprocess shutdown complete."
}

# =============================================================================
# Initialize Database
# =============================================================================

# Create db directory if needed
set db_dir [file dirname $::docs_db_path]
if {![file exists $db_dir]} {
    file mkdir $db_dir
}

# Check if database file exists and is valid
set needs_setup 0

if {[file exists $::docs_db_path]} {
    # File exists - try to open and check if schema is present
    if {$::docs_authoring_mode} {
        docsdb::init $::docs_db_path readwrite
        puts "Documentation database opened (read-write authoring mode): $::docs_db_path"
    } else {
        # Try read-only first
        if {[catch {docsdb::init $::docs_db_path readonly} err]} {
            # Failed to open read-only, try read-write
            puts "Could not open read-only, trying read-write: $err"
            docsdb::init $::docs_db_path readwrite
        } else {
            puts "Documentation database opened (read-only): $::docs_db_path"
        }
    }
    
    # Check if properly set up
    if {![docsdb::is_setup]} {
        puts "Database exists but is not set up - running setup..."
        # Need to reopen as read-write if we opened read-only
        if {!$::docs_authoring_mode} {
            docsdb::close
            docsdb::init $::docs_db_path readwrite
        }
        set needs_setup 1
    }
} else {
    # No database file - create new
    puts "No documentation database found at $::docs_db_path"
    puts "Creating new database..."
    docsdb::init $::docs_db_path readwrite
    set needs_setup 1
}

if {$needs_setup} {
    docsdb::setup
    puts ""
    puts "Database created. To populate with commands, run:"
    puts "  ingest_all_commands"
    puts ""
    puts "Then restart dserv to open in read-only mode."
} else {
    set stats [docsdb::stats]
    dict for {key val} $stats {
        puts "  $key: $val"
    }
}

# =============================================================================
# Authoring Mode Toggle
# =============================================================================
# Content editor can call these to switch modes

proc docs_enable_authoring {} {
    if {$::docs_authoring_mode} {
        return [::docsapi::json [dict create status "ok" message "Already in authoring mode"]]
    }
    
    # Auto-backup before enabling writes (keep last 3)
    if {[file exists $::docs_db_path]} {
        set backup_dir [file dirname $::docs_db_path]
        set backup_base [file tail $::docs_db_path]
        
        # Find existing backups and remove old ones
        set backups [lsort [glob -nocomplain -directory $backup_dir "${backup_base}.backup-*"]]
        while {[llength $backups] >= 3} {
            file delete [lindex $backups 0]
            set backups [lrange $backups 1 end]
        }
        
        # Create new backup
        set timestamp [clock format [clock seconds] -format %Y%m%d-%H%M%S]
        set backup "${::docs_db_path}.backup-${timestamp}"
        file copy $::docs_db_path $backup
        puts "Backup created: $backup"
    }
    
    docsdb::close
    docsdb::init $::docs_db_path readwrite
    set ::docs_authoring_mode 1
    return [::docsapi::json [dict create status "ok" message "Switched to authoring mode (read-write)"]]
}

proc docs_authoring_status {} {
    return [::docsapi::json [dict create authoring $::docs_authoring_mode]]
}

# =============================================================================
# Convenience Procedures
# =============================================================================
# These provide simpler names for common frontend patterns

# Tutorial system
proc get_lessons_for_category {category} {
    return [api_entries_list [dict create type tutorial category $category]]
}

proc get_lesson {slug} {
    return [api_entry_get $slug]
}

# Command reference
proc get_commands {{namespace ""}} {
    if {$namespace eq "" || $namespace eq "all"} {
        return [api_commands_list]
    }
    return [api_commands_list [dict create namespace $namespace]]
}

proc get_command {name} {
    return [api_command_get $name]
}

proc get_command_namespaces {} {
    return [api_namespaces_list]
}

# =============================================================================
# Administrative Commands
# =============================================================================
# These are for content management, not typically called from web UI

proc ingest_all_commands {} {
    # Ingest commands from live interpreter
    # Run this after loading all your C extensions
    
    puts "Ingesting commands from live interpreter..."
    
    set total_imported 0
    set total_skipped 0
    
    # Add patterns for your command sets
    foreach {pattern ns} {
        "dl_*"      dl
        "dg_*"      dg
        "ess::*"    ess
        "gpio_*"    gpio
        "sound_*"   sound
        "qpcs_*"    qpcs
    } {
        if {[llength [info commands $pattern]] > 0} {
            puts "  Processing $pattern..."
            set result [docsdb::ingest_commands $pattern $ns]
            incr total_imported [dict get $result imported]
            incr total_skipped [dict get $result skipped]
        }
    }
    
    puts "Total: $total_imported imported, $total_skipped skipped"
    return [dict create imported $total_imported skipped $total_skipped]
}

proc rebuild_fts_index {} {
    # Rebuild full-text search index (rarely needed)
    puts "Rebuilding FTS index..."
    docsdb::db eval {INSERT INTO entries_fts(entries_fts) VALUES('rebuild')}
    puts "Done"
}

proc docs_help {} {
    puts {
Documentation Subprocess Commands
=================================

Query API (for web frontends):
  api_entries_list ?filter?     - List entries (filter: type, category, namespace)
  api_entry_get slug            - Get single entry with all details
  api_commands_list ?filter?    - List commands only
  api_command_get name          - Get command details  
  api_tutorials_list ?filter?   - List tutorials only
  api_categories_list           - List all categories
  api_namespaces_list           - List command namespaces with counts
  api_search {query "text"}     - Full-text search
  api_quick_help cmd_name       - Minimal info for autocomplete
  api_stats                     - Database statistics

Convenience wrappers:
  get_commands ?namespace?      - List commands, optionally filtered
  get_command name              - Get command details
  get_lessons_for_category cat  - Get tutorials in category
  get_lesson slug               - Get tutorial details

Administration:
  ingest_all_commands           - Import commands from interpreter
  rebuild_fts_index             - Rebuild search index
  docsdb::stats                 - Show database statistics

Content authoring:
  api_entry_create data         - Create new entry
  api_entry_update slug data    - Update entry
  api_entry_delete slug         - Delete entry
    }
}

# =============================================================================
# Ready
# =============================================================================

puts ""
puts "Documentation subprocess ready. Type 'docs_help' for commands."
