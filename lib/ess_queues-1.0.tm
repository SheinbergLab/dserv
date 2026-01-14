# ess_queues-1_0.tm -- Queue management and run orchestration for ESS
#
# Manages experiment queues (ordered sequences of configs) and orchestrates
# running through them with datafile management, pauses, and manual control.
#
# Part of the configs subprocess - shares database with ess_configs module.
#

package provide ess_queues 1.0

namespace eval ::ess_queues {
    # Database handle (shared with ess_configs)
    variable db ""
    
    # Current queue state
    variable state
    array set state {
        status          idle
        queue_name      ""
        position        0
        total_items     0
        current_config  ""
        run_count       0
        global_run      0
        pause_until     0
        flush_until     0
        run_started     0
        current_repeat_count 1
        current_pause_after  0
        auto_start      1
        auto_advance    1
        auto_datafile   1
        datafile_open   0
    }
    
    # Status values:
    #   idle         - no queue active
    #   loading      - loading next config
    #   ready        - config loaded, waiting to start (manual) or about to auto-start
    #   running      - ESS is running
    #   flushing     - run complete, waiting for datapoints to flush before closing file
    #   paused       - run stopped early, waiting for retry/skip/abort
    #   between_runs - run complete, in delay or waiting for manual advance
    #   finished     - queue complete
    
    # Flush delay in milliseconds before closing datafile
    variable flush_delay_ms 750
}

# =============================================================================
# Initialization
# =============================================================================

proc ::ess_queues::init {db_handle} {
    variable db
    set db $db_handle
    
    create_tables
    publish_state
    
    return 1
}

proc ::ess_queues::create_tables {} {
    variable db
    
    # Queues table - base schema
    $db eval {
        CREATE TABLE IF NOT EXISTS queues (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT UNIQUE NOT NULL,
            description TEXT DEFAULT '',
            auto_start INTEGER DEFAULT 1,
            auto_advance INTEGER DEFAULT 1,
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            created_by TEXT DEFAULT '',
            updated_at INTEGER DEFAULT (strftime('%s', 'now'))
        )
    }
    
    # Queue items table
    $db eval {
        CREATE TABLE IF NOT EXISTS queue_items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            queue_id INTEGER NOT NULL,
            config_name TEXT NOT NULL,
            position INTEGER NOT NULL,
            repeat_count INTEGER DEFAULT 1,
            pause_after INTEGER DEFAULT 0,
            notes TEXT DEFAULT '',
            FOREIGN KEY (queue_id) REFERENCES queues(id) ON DELETE CASCADE,
            UNIQUE(queue_id, position)
        )
    }
    
    # Index for efficient lookups
    $db eval {
        CREATE INDEX IF NOT EXISTS idx_queue_items_queue_id 
        ON queue_items(queue_id, position)
    }
    
    # Run queue-specific migrations
    migrate_queues
}

#=========================================================================
# Queue Schema Migrations
#=========================================================================

# Check if a column exists in a table
proc ::ess_queues::column_exists {table column} {
    variable db
    set cols [$db eval "PRAGMA table_info($table)"]
    foreach {cid name type notnull dflt pk} $cols {
        if {$name eq $column} {
            return 1
        }
    }
    return 0
}

# Run queue-specific migrations
# Note: We use ess_configs' schema_version for the shared database,
# but we also need to handle queue-specific columns
proc ::ess_queues::migrate_queues {} {
    variable db
    
    # Migration: Add auto_datafile column to queues
    if {![column_exists queues auto_datafile]} {
        log info "Migration: Adding auto_datafile column to queues"
        $db eval {
            ALTER TABLE queues ADD COLUMN auto_datafile INTEGER DEFAULT 1
        }
    }
    
    # Migration: Add datafile_template column to queues
    if {![column_exists queues datafile_template]} {
        log info "Migration: Adding datafile_template column to queues"
        $db eval {
            ALTER TABLE queues ADD COLUMN datafile_template TEXT DEFAULT '{suggest}'
        }
    }
}

# =============================================================================
# Datafile Naming 
# =============================================================================

# Generate datafile basename from queue's template
# Returns just the basename - ESS handles path and extension
proc ::ess_queues::generate_datafile_basename {} {
    variable state
    variable db
    
    # Get the queue's template setting
    set queue_id [get_queue_id $state(queue_name)]
    set template [$db onecolumn {
        SELECT datafile_template FROM queues WHERE id = :queue_id
    }]
    
    # If template is {suggest} or empty, defer to ESS's file_suggest
    if {$template eq "{suggest}" || $template eq ""} {
        set basename [send ess "::ess::file_suggest"]
        # Append run number if we have repeats
        if {$state(current_repeat_count) > 1} {
            append basename "_r$state(run_count)"
        }
        return $basename
    }
    
    # Otherwise, process the template
    
    # Get values from ESS state
    set subject [dservGet ess/subject]
    if {$subject eq ""} { set subject "unknown" }
    
    set system [dservGet ess/system]
    set protocol [dservGet ess/protocol]
    set variant [dservGet ess/variant]
    
    # Get config's short_name if available
    set config_label $state(current_config)
    if {[catch {
        set config_info [::ess::configs::get $state(current_config)]
        if {$config_info ne "" && [dict exists $config_info short_name]} {
            set sn [dict get $config_info short_name]
            if {$sn ne ""} {
                set config_label $sn
            }
        }
    }]} {
        # Failed to get config info, use config name as-is
    }
    
    # Format date and time
    set now [clock seconds]
    set date [clock format $now -format "%Y%m%d"]
    set date_short [clock format $now -format "%y%m%d"]
    set time [clock format $now -format "%H%M%S"]
    set time_short [clock format $now -format "%H%M"]
    set timestamp $now
    
    # Clean strings for filename (remove special chars)
    set config_clean [regsub -all {[^a-zA-Z0-9_-]} $config_label "_"]
    set queue_clean [regsub -all {[^a-zA-Z0-9_-]} $state(queue_name) "_"]
    set subject_clean [regsub -all {[^a-zA-Z0-9_-]} $subject "_"]
    
    # Build substitution map for string variables
    set subst_map [list \
        "{subject}"   $subject_clean \
        "{system}"    $system \
        "{protocol}"  $protocol \
        "{variant}"   $variant \
        "{date}"      $date \
        "{date_short}" $date_short \
        "{time}"      $time \
        "{time_short}" $time_short \
        "{timestamp}" $timestamp \
        "{config}"    $config_clean \
        "{queue}"     $queue_clean]
    
    # Start with template
    set basename $template
    
    # Handle numeric variables with optional format specifier {var:NN}
    foreach {var value} [list position $state(position) run $state(run_count) global $state(global_run)] {
        # Handle formatted version {var:NN} - zero-padded to NN digits
        while {[regexp "\\{${var}:(\\d+)\\}" $basename -> width]} {
            set formatted [format "%0${width}d" $value]
            regsub "\\{${var}:\\d+\\}" $basename $formatted basename
        }
        # Handle plain version {var}
        set basename [string map [list "\{$var\}" $value] $basename]
    }
    
    # Perform string substitutions
    set basename [string map $subst_map $basename]
    
    return $basename
}

# =============================================================================
# ESS Control Integration
# =============================================================================

# Open datafile for current run
proc ::ess_queues::open_datafile {} {
    variable state
    
    # Check if auto_datafile is enabled for this queue
    if {!$state(auto_datafile)} {
        log info "auto_datafile disabled - skipping file open"
        return 1
    }
    
    if {$state(datafile_open)} {
        log warning "Datafile already open - closing first"
        close_datafile
    }
    
    set basename [generate_datafile_basename]
    
    log info "Opening datafile: $basename"
    
    if {[catch {send ess "::ess::file_open {$basename}"} result]} {
        log error "Failed to open datafile: $result"
        return 0
    }
    
    # Check return value from ess::file_open
    # Returns: 1 = success, 0 = file exists, -1 = file already open
    if {$result == 0} {
        log error "Datafile already exists: $basename"
        return 0
    } elseif {$result == -1} {
        log error "Another datafile is already open"
        return 0
    }
    
    set state(datafile_open) 1
    dservSet queues/datafile $basename
    return 1
}

# Close current datafile
proc ::ess_queues::close_datafile {} {
    variable state
    
    # If auto_datafile is disabled, nothing to close
    if {!$state(auto_datafile)} {
        return 1
    }
    
    if {!$state(datafile_open)} {
        return 1
    }
    
    log info "Closing datafile"
    
    if {[catch {send ess "::ess::file_close"} err]} {
        log warning "Error closing datafile: $err"
    }
    
    set state(datafile_open) 0
    dservSet queues/datafile ""
    return 1
}

# Start ESS
proc ::ess_queues::ess_start {} {
    log info "Starting ESS"
    if {[catch {send ess "ess::start"} err]} {
        log error "Failed to start ESS: $err"
        return 0
    }
    return 1
}

# Stop ESS
proc ::ess_queues::ess_stop {} {
    log info "Stopping ESS"
    if {[catch {send ess "ess::stop"} err]} {
        log warning "Error stopping ESS: $err"
    }
    return 1
}

# =============================================================================
# Logging
# =============================================================================

proc ::ess_queues::log {level msg} {
    set timestamp [clock format [clock seconds] -format "%H:%M:%S"]
    set full_msg "\[$timestamp\] \[$level\] $msg"
    
    # Publish to datapoint for monitoring
    if {[catch {dservSet queues/log $full_msg}]} {
        puts "ess_queues $level: $msg"
    }
}

# =============================================================================
# Queue CRUD Operations
# =============================================================================

proc ::ess_queues::queue_create {name args} {
    variable db
    
    # Parse optional arguments
    set description ""
    set auto_start 1
    set auto_advance 1
    set auto_datafile 1
    set datafile_template "{suggest}"
    set created_by ""
    
    foreach {opt val} $args {
        switch -- $opt {
            -description { set description $val }
            -auto_start { set auto_start [expr {$val ? 1 : 0}] }
            -auto_advance { set auto_advance [expr {$val ? 1 : 0}] }
            -auto_datafile { set auto_datafile [expr {$val ? 1 : 0}] }
            -datafile_template { set datafile_template $val }
            -created_by { set created_by $val }
        }
    }
    
    # Validate name
    if {![regexp {^[\w\-\.]+$} $name]} {
        error "Invalid queue name: use only letters, numbers, underscores, dashes, dots"
    }
    
    # Check for duplicate
    if {[$db exists {SELECT 1 FROM queues WHERE name = :name}]} {
        error "Queue already exists: $name"
    }
    
    $db eval {
        INSERT INTO queues (name, description, auto_start, auto_advance, 
                           auto_datafile, datafile_template, created_by)
        VALUES (:name, :description, :auto_start, :auto_advance,
                :auto_datafile, :datafile_template, :created_by)
    }
    
    log info "Created queue: $name"
    publish_list
    return $name
}

proc ::ess_queues::queue_delete {name} {
    variable db
    variable state
    
    # Can't delete active queue
    if {$state(status) ne "idle" && $state(queue_name) eq $name} {
        error "Cannot delete active queue"
    }
    
    set queue_id [get_queue_id $name]
    
    # Delete items first (cascade should handle this, but be explicit)
    $db eval {DELETE FROM queue_items WHERE queue_id = :queue_id}
    $db eval {DELETE FROM queues WHERE id = :queue_id}
    
    log info "Deleted queue: $name"
    publish_list
    return $name
}

proc ::ess_queues::queue_list {} {
    variable db
    
    set result [list]
    $db eval {
        SELECT q.name, q.description, q.auto_start, q.auto_advance,
               q.auto_datafile, q.datafile_template,
               q.created_at, q.created_by,
               (SELECT COUNT(*) FROM queue_items WHERE queue_id = q.id) as item_count
        FROM queues q
        ORDER BY q.name
    } row {
        lappend result [dict create \
            name $row(name) \
            description $row(description) \
            auto_start $row(auto_start) \
            auto_advance $row(auto_advance) \
            auto_datafile $row(auto_datafile) \
            datafile_template $row(datafile_template) \
            item_count $row(item_count) \
            created_at $row(created_at) \
            created_by $row(created_by)]
    }
    
    return $result
}

proc ::ess_queues::queue_get {name} {
    variable db
    
    set queue_id [get_queue_id $name]
    
    # Get queue info
    set queue_info [$db eval {
        SELECT name, description, auto_start, auto_advance, 
               auto_datafile, datafile_template, created_at, created_by
        FROM queues WHERE id = :queue_id
    }]
    
    if {$queue_info eq ""} {
        error "Queue not found: $name"
    }
    
    lassign $queue_info qname desc auto_start auto_advance auto_datafile datafile_template created_at created_by
    
    # Get items
    set items [list]
    $db eval {
        SELECT config_name, position, repeat_count, pause_after, notes
        FROM queue_items
        WHERE queue_id = :queue_id
        ORDER BY position
    } row {
        lappend items [dict create \
            config_name $row(config_name) \
            position $row(position) \
            repeat_count $row(repeat_count) \
            pause_after $row(pause_after) \
            notes $row(notes)]
    }
    
    return [dict create \
        name $qname \
        description $desc \
        auto_start $auto_start \
        auto_advance $auto_advance \
        auto_datafile $auto_datafile \
        datafile_template $datafile_template \
        items $items \
        created_at $created_at \
        created_by $created_by]
}

proc ::ess_queues::queue_update {name args} {
    variable db
    
    set queue_id [get_queue_id $name]
    
    foreach {opt val} $args {
        switch -- $opt {
            -description {
                $db eval {UPDATE queues SET description = :val, updated_at = strftime('%s','now') WHERE id = :queue_id}
            }
            -auto_start {
                set val [expr {$val ? 1 : 0}]
                $db eval {UPDATE queues SET auto_start = :val, updated_at = strftime('%s','now') WHERE id = :queue_id}
            }
            -auto_advance {
                set val [expr {$val ? 1 : 0}]
                $db eval {UPDATE queues SET auto_advance = :val, updated_at = strftime('%s','now') WHERE id = :queue_id}
            }
            -auto_datafile {
                set val [expr {$val ? 1 : 0}]
                $db eval {UPDATE queues SET auto_datafile = :val, updated_at = strftime('%s','now') WHERE id = :queue_id}
            }
            -datafile_template {
                $db eval {UPDATE queues SET datafile_template = :val, updated_at = strftime('%s','now') WHERE id = :queue_id}
            }
            -name {
                if {![regexp {^[\w\-\.]+$} $val]} {
                    error "Invalid queue name"
                }
                $db eval {UPDATE queues SET name = :val, updated_at = strftime('%s','now') WHERE id = :queue_id}
                set name $val
            }
        }
    }
    
    publish_list
    return $name
}

# =============================================================================
# Queue Item Management
# =============================================================================

proc ::ess_queues::queue_add {queue_name config_name args} {
    variable db
    
    set queue_id [get_queue_id $queue_name]
    
    # Parse options
    set position -1
    set repeat_count 1
    set pause_after 0
    set notes ""
    
    foreach {opt val} $args {
        switch -- $opt {
            -position { set position $val }
            -repeat { set repeat_count [expr {int($val)}] }
            -pause_after { set pause_after [expr {int($val)}] }
            -notes { set notes $val }
        }
    }
    
    # Get max position if not specified
    if {$position < 0} {
        set max_pos [$db onecolumn {
            SELECT COALESCE(MAX(position), -1) FROM queue_items WHERE queue_id = :queue_id
        }]
        set position [expr {$max_pos + 1}]
    } else {
        # Shift existing items to make room
        $db eval {
            UPDATE queue_items 
            SET position = position + 1 
            WHERE queue_id = :queue_id AND position >= :position
        }
    }
    
    $db eval {
        INSERT INTO queue_items (queue_id, config_name, position, repeat_count, pause_after, notes)
        VALUES (:queue_id, :config_name, :position, :repeat_count, :pause_after, :notes)
    }
    
    publish_queue_items $queue_name
    return $position
}

proc ::ess_queues::queue_remove {queue_name position} {
    variable db
    
    set queue_id [get_queue_id $queue_name]
    set position [expr {int($position)}]
    
    # Delete the item
    $db eval {
        DELETE FROM queue_items WHERE queue_id = :queue_id AND position = :position
    }
    
    # Renumber remaining items
    renumber_items $queue_id
    
    publish_queue_items $queue_name
}

proc ::ess_queues::queue_reorder {queue_name from_pos to_pos} {
    variable db
    
    set queue_id [get_queue_id $queue_name]
    set from_pos [expr {int($from_pos)}]
    set to_pos [expr {int($to_pos)}]
    
    if {$from_pos == $to_pos} return
    
    # Get the item being moved
    set item_id [$db onecolumn {
        SELECT id FROM queue_items WHERE queue_id = :queue_id AND position = :from_pos
    }]
    
    if {$item_id eq ""} {
        error "No item at position $from_pos"
    }
    
    # Temporarily move to position -1
    $db eval {UPDATE queue_items SET position = -1 WHERE id = :item_id}
    
    # Shift items
    if {$from_pos < $to_pos} {
        # Moving down - shift items up
        $db eval {
            UPDATE queue_items 
            SET position = position - 1 
            WHERE queue_id = :queue_id AND position > :from_pos AND position <= :to_pos
        }
    } else {
        # Moving up - shift items down
        $db eval {
            UPDATE queue_items 
            SET position = position + 1 
            WHERE queue_id = :queue_id AND position >= :to_pos AND position < :from_pos
        }
    }
    
    # Place item in new position
    $db eval {UPDATE queue_items SET position = :to_pos WHERE id = :item_id}
    
    publish_queue_items $queue_name
}

proc ::ess_queues::queue_clear {queue_name} {
    variable db
    
    set queue_id [get_queue_id $queue_name]
    
    $db eval {DELETE FROM queue_items WHERE queue_id = :queue_id}
    
    publish_queue_items $queue_name
}

proc ::ess_queues::queue_item_update {queue_name position args} {
    variable db
    
    set queue_id [get_queue_id $queue_name]
    
    foreach {opt val} $args {
        switch -- $opt {
            -config_name {
                $db eval {
                    UPDATE queue_items SET config_name = :val 
                    WHERE queue_id = :queue_id AND position = :position
                }
            }
            -repeat {
                set val [expr {int($val)}]
                $db eval {
                    UPDATE queue_items SET repeat_count = :val 
                    WHERE queue_id = :queue_id AND position = :position
                }
            }
            -pause_after {
                set val [expr {int($val)}]
                $db eval {
                    UPDATE queue_items SET pause_after = :val 
                    WHERE queue_id = :queue_id AND position = :position
                }
            }
            -notes {
                $db eval {
                    UPDATE queue_items SET notes = :val 
                    WHERE queue_id = :queue_id AND position = :position
                }
            }
        }
    }
    
    publish_queue_items $queue_name
}

# =============================================================================
# Queue Run Orchestration
# =============================================================================

proc ::ess_queues::queue_start {queue_name args} {
    variable state
    variable db
    
    # Can't start if already running
    if {$state(status) ni {idle finished}} {
        error "Queue already active: $state(queue_name)"
    }
    
    # Parse options
    set start_position 0
    foreach {opt val} $args {
        switch -- $opt {
            -position { set start_position [expr {int($val)}] }
        }
    }
    
    # Validate queue exists and has items
    set queue_id [get_queue_id $queue_name]
    set item_count [$db onecolumn {
        SELECT COUNT(*) FROM queue_items WHERE queue_id = :queue_id
    }]
    
    if {$item_count == 0} {
        error "Queue is empty: $queue_name"
    }
    
    # Get queue settings
    lassign [$db eval {
        SELECT auto_start, auto_advance, auto_datafile FROM queues WHERE id = :queue_id
    }] auto_start auto_advance auto_datafile
    
    # Initialize state
    set state(status) loading
    set state(queue_name) $queue_name
    set state(position) $start_position
    set state(total_items) $item_count
    set state(run_count) 0
    set state(global_run) 0
    set state(current_config) ""
    set state(pause_until) 0
    set state(run_started) 0
    set state(datafile_open) 0
    
    # Store queue settings in state for easy access
    set state(auto_start) $auto_start
    set state(auto_advance) $auto_advance
    set state(auto_datafile) $auto_datafile
    
    log info "Starting queue: $queue_name (items: $item_count, position: $start_position, auto_datafile: $auto_datafile)"
    publish_state
    
    # Load the first config
    load_current_item
    
    return $queue_name
}

proc ::ess_queues::queue_stop {} {
    variable state
    
    if {$state(status) eq "idle"} {
        return
    }
    
    log info "Stopping queue: $state(queue_name)"
    
    # Stop ESS if running
    if {$state(status) eq "running"} {
        ess_stop
    }
    
    # Close any open datafile
    close_datafile
    
    # Reset state
    set state(status) idle
    set state(queue_name) ""
    set state(position) 0
    set state(total_items) 0
    set state(current_config) ""
    set state(run_count) 0
    set state(pause_until) 0
    set state(run_started) 0
    
    publish_state
}

proc ::ess_queues::queue_pause {} {
    variable state
    
    # Only valid when running or ready
    if {$state(status) ni {running ready}} {
        error "Cannot pause: queue is $state(status)"
    }
    
    log info "Pausing queue at position $state(position)"
    
    # If running, stop the current run
    if {$state(status) eq "running"} {
        ess_stop
        close_datafile
    }
    
    set state(status) paused
    publish_state
}

proc ::ess_queues::queue_resume {} {
    variable state
    
    if {$state(status) ne "paused"} {
        error "Cannot resume: queue is $state(status)"
    }
    
    log info "Resuming queue from position $state(position)"
    
    # Go back to ready state, which will trigger start on next tick if auto_start
    set state(status) ready
    publish_state
    
    if {$state(auto_start)} {
        start_run
    }
}

proc ::ess_queues::queue_run {} {
    variable state
    
    # Manually start the run when in ready state
    if {$state(status) ne "ready"} {
        error "Cannot run: queue is $state(status)"
    }
    
    start_run
}

proc ::ess_queues::queue_next {} {
    variable state
    
    # Manual advance - complete current run and move on (respects repeats)
    if {$state(status) eq "idle"} {
        error "No active queue"
    }
    
    log info "Manual next: position $state(position), run $state(run_count)/$state(current_repeat_count)"
    
    # Stop if currently running
    if {$state(status) eq "running"} {
        ess_stop
        close_datafile
    }
    
    set state(run_started) 0
    
    # Check if more repeats needed
    if {$state(run_count) < $state(current_repeat_count)} {
        # More repeats - stay on same item, go to ready
        set state(status) ready
        publish_state
        
        if {$state(auto_start)} {
            start_run
        }
        return
    }
    
    # Done with repeats, advance to next item
    advance_to_next
}

proc ::ess_queues::queue_skip {} {
    variable state
    
    # Skip current item entirely (even remaining repeats)
    if {$state(status) eq "idle"} {
        error "No active queue"
    }
    
    log info "Skipping position $state(position)"
    
    # Stop if currently running
    if {$state(status) eq "running"} {
        ess_stop
        close_datafile
    }
    
    set state(run_count) 0  ;# Reset repeat counter
    advance_to_next
}

proc ::ess_queues::queue_retry {} {
    variable state
    
    # Reload and retry current item
    if {$state(status) ni {paused between_runs}} {
        error "Cannot retry: queue is $state(status)"
    }
    
    log info "Retrying position $state(position)"
    
    set state(status) loading
    publish_state
    
    load_current_item
}

# =============================================================================
# Orchestration Internals
# =============================================================================

proc ::ess_queues::load_current_item {} {
    variable state
    variable db
    
    set queue_id [get_queue_id $state(queue_name)]
    set pos $state(position)
    
    # Get current item
    set item [$db eval {
        SELECT config_name, repeat_count, pause_after
        FROM queue_items
        WHERE queue_id = :queue_id AND position = :pos
    }]
    
    if {$item eq ""} {
        # No more items - queue finished
        log info "Queue finished: $state(queue_name)"
        set state(status) finished
        set state(current_config) ""
        publish_state
        return
    }
    
    lassign $item config_name repeat_count pause_after
    
    set state(current_config) $config_name
    set state(current_repeat_count) $repeat_count
    set state(current_pause_after) $pause_after
    
    log info "Loading config: $config_name (position $pos, repeats: $repeat_count)"
    
    # Load the config via ess_configs
    if {[catch {::ess::configs::load $config_name} err]} {
        log error "Failed to load config '$config_name': $err"
        set state(status) paused
        publish_state
        return
    }
    
    # Config loaded successfully - transition to ready
    set state(status) ready
    publish_state
    
    # If auto_start, begin the run
    if {$state(auto_start)} {
        start_run
    }
}

proc ::ess_queues::start_run {} {
    variable state
    
    if {$state(status) ne "ready"} {
        return
    }
    
    incr state(run_count)
    incr state(global_run)
    log info "Starting run $state(run_count)/$state(current_repeat_count) for $state(current_config) (global: $state(global_run))"
    
    # Open datafile
    if {![open_datafile]} {
        log error "Failed to open datafile - pausing queue"
        set state(status) paused
        publish_state
        return
    }
    
    # Start ESS
    if {![ess_start]} {
        log error "Failed to start ESS - pausing queue"
        close_datafile
        set state(status) paused
        publish_state
        return
    }
    
    set state(status) running
    set state(run_started) 1
    publish_state
}

proc ::ess_queues::on_run_complete {} {
    variable state
    variable flush_delay_ms
    
    if {$state(status) ne "running"} {
        return
    }
    
    log info "Run complete: position $state(position), run $state(run_count)/$state(current_repeat_count)"
    
    # Enter flushing state - wait for datapoints to flush before closing file
    set state(status) flushing
    set state(flush_until) [expr {[clock milliseconds] + $flush_delay_ms}]
    publish_state
}

# Called after flush delay to actually close file and continue
proc ::ess_queues::finish_run_complete {} {
    variable state
    
    log info "Flush complete, closing datafile"
    
    # Close datafile
    close_datafile
    
    # Check if more repeats needed
    if {$state(run_count) < $state(current_repeat_count)} {
        # More repeats - stay on same item
        log info "More repeats needed ($state(run_count)/$state(current_repeat_count))"
        set state(status) ready
        publish_state
        
        if {$state(auto_start)} {
            start_run
        }
        return
    }
    
    # Check for pause_after delay
    if {$state(current_pause_after) > 0} {
        set state(status) between_runs
        set state(pause_until) [expr {[clock seconds] + $state(current_pause_after)}]
        log info "Pausing for $state(current_pause_after) seconds before next item"
        publish_state
        return
    }
    
    # Auto-advance or wait
    if {$state(auto_advance)} {
        advance_to_next
    } else {
        set state(status) between_runs
        set state(pause_until) 0
        log info "Waiting for manual advance"
        publish_state
    }
}

# Force complete the current run (for ending early)
# This triggers the normal completion flow - flush, close file, advance
proc ::ess_queues::force_complete {} {
    variable state
    
    if {$state(status) ne "running"} {
        log warning "force_complete: not running (status=$state(status))"
        return
    }
    
    log info "Force completing run at position $state(position)"
    
    # Stop ESS if it's running
    ess_stop
    
    # Trigger normal completion flow
    set state(run_started) 0
    on_run_complete
}

proc ::ess_queues::advance_to_next {} {
    variable state
    
    set state(run_count) 0
    incr state(position)
    
    log info "Advancing to position $state(position)/$state(total_items)"
    
    if {$state(position) >= $state(total_items)} {
        # Queue complete
        log info "Queue complete: $state(queue_name)"
        set state(status) finished
        set state(current_config) ""
        publish_state
        return
    }
    
    set state(status) loading
    publish_state
    
    load_current_item
}

# Called by ess/run_state subscription
proc ::ess_queues::on_ess_run_state {run_state} {
    variable state
    
    # Only react if we're actually running a queue
    if {$state(status) ne "running"} {
        return
    }
    
    # Run complete when ESS reaches end state
    # run_state is "active" when running, "complete" when finished normally
    if {$run_state eq "complete" && $state(run_started)} {
        set state(run_started) 0
        on_run_complete
    }
}

# Called periodically by dserv timer
proc ::ess_queues::tick {} {
    variable state
    
    # Handle flush delay before closing datafile
    if {$state(status) eq "flushing"} {
        if {[clock milliseconds] >= $state(flush_until)} {
            set state(flush_until) 0
            finish_run_complete
        }
        return
    }
    
    # Handle pause_until delay
    if {$state(status) eq "between_runs" && $state(pause_until) > 0} {
        set now [clock seconds]
        set remaining [expr {$state(pause_until) - $now}]
        
        if {$remaining <= 0} {
            log info "Pause complete - advancing"
            set state(pause_until) 0
            
            if {$state(auto_advance)} {
                advance_to_next
            }
        } else {
            # Publish remaining time for UI
            dservSet queues/pause_remaining $remaining
        }
    }
}

# =============================================================================
# State Publishing
# =============================================================================

proc ::ess_queues::publish_state {} {
    variable state
    
    # Publish individual state datapoints
    dservSet queues/status $state(status)
    dservSet queues/active $state(queue_name)
    dservSet queues/position $state(position)
    dservSet queues/total $state(total_items)
    dservSet queues/current_config $state(current_config)
    dservSet queues/run_count $state(run_count)
    dservSet queues/repeat_total $state(current_repeat_count)
    dservSet queues/global_run $state(global_run)
    
    # Calculate pause_remaining if applicable
    set pause_remaining 0
    if {$state(pause_until) > 0} {
        set pause_remaining [expr {max(0, $state(pause_until) - [clock seconds])}]
    }
    dservSet queues/pause_remaining $pause_remaining
    
    # Publish combined state as JSON for UI
    set json_state [dict create \
        status $state(status) \
        queue_name $state(queue_name) \
        position $state(position) \
        total_items $state(total_items) \
        current_config $state(current_config) \
        run_count $state(run_count) \
        repeat_total $state(current_repeat_count) \
        pause_remaining $pause_remaining \
        auto_start $state(auto_start) \
        auto_advance $state(auto_advance) \
        global_run $state(global_run)]
    
    dservSet queues/state [dict_to_json $json_state]
}

proc ::ess_queues::publish_list {} {
    variable db
    
    set queues [queue_list]
    dservSet queues/list [list_to_json $queues]
}

proc ::ess_queues::publish_queue_items {queue_name} {
    variable db
    variable state
    
    # Only publish if this is the active queue
    if {$state(queue_name) eq $queue_name} {
        set queue [queue_get $queue_name]
        dservSet queues/items [list_to_json [dict get $queue items]]
    }
}

# =============================================================================
# Export/Import for Cross-Rig Sync
# =============================================================================

# Export queue as JSON
# Usage: queue_export "name" ?-include_configs?
# Returns JSON with queue definition and optionally bundled config exports
proc ::ess_queues::queue_export {queue_name args} {
    variable db
    
    # Parse options
    set include_configs 0
    foreach arg $args {
        switch -- $arg {
            -include_configs { set include_configs 1 }
        }
    }
    
    # Get queue info
    set queue_id [get_queue_id $queue_name]
    
    set queue_data [$db eval {
        SELECT name, description, auto_start, auto_advance, auto_datafile, 
               datafile_template, created_by
        FROM queues WHERE id = :queue_id
    }]
    
    if {$queue_data eq ""} {
        error "Queue not found: $queue_name"
    }
    
    lassign $queue_data name description auto_start auto_advance auto_datafile \
                        datafile_template created_by
    
    # Get items
    set items [list]
    $db eval {
        SELECT config_name, position, repeat_count, pause_after, notes
        FROM queue_items 
        WHERE queue_id = :queue_id 
        ORDER BY position
    } row {
        lappend items [dict create \
            config_name $row(config_name) \
            position $row(position) \
            repeat_count $row(repeat_count) \
            pause_after $row(pause_after) \
            notes $row(notes)]
    }
    
    # Build export dict
    set export_dict [dict create \
        name $name \
        description $description \
        auto_start $auto_start \
        auto_advance $auto_advance \
        auto_datafile $auto_datafile \
        datafile_template $datafile_template \
        created_by $created_by \
        items $items]
    
    # Build JSON using yajltcl for proper encoding
    package require yajltcl
    set obj [yajl create #auto]
    
    $obj map_open
    $obj string "queue" map_open
    
    dict for {k v} $export_dict {
        if {$k eq "items"} {
            $obj string "items" array_open
            foreach item $v {
                $obj map_open
                dict for {ik iv} $item {
                    if {[string is integer -strict $iv]} {
                        $obj string $ik number $iv
                    } else {
                        $obj string $ik string $iv
                    }
                }
                $obj map_close
            }
            $obj array_close
        } elseif {[string is integer -strict $v]} {
            $obj string $k number $v
        } else {
            $obj string $k string $v
        }
    }
    
    $obj map_close
    
    # Optionally include configs
    if {$include_configs} {
        $obj string "configs" array_open
        
        # Get unique config names from items
        set config_names [list]
        foreach item $items {
            set cname [dict get $item config_name]
            if {$cname ni $config_names} {
                lappend config_names $cname
            }
        }
        
        # Export each config
        foreach cname $config_names {
            if {[catch {
                set config_json [::ess::configs::export $cname]
                # Parse and re-emit to include in our JSON structure
                set config_dict [json_to_dict $config_json]
                $obj map_open
                dict for {ck cv} $config_dict {
                    if {[llength $cv] > 1 && ![string is integer -strict $cv]} {
                        # Nested dict or list - serialize as string for simplicity
                        $obj string $ck string $cv
                    } elseif {[string is integer -strict $cv]} {
                        $obj string $ck number $cv
                    } else {
                        $obj string $ck string $cv
                    }
                }
                $obj map_close
            } err]} {
                log warning "Could not export config '$cname': $err"
            }
        }
        
        $obj array_close
    }
    
    $obj map_close
    
    set result [$obj get]
    $obj delete
    
    return $result
}

# Import queue from JSON
# Usage: queue_import {json} ?-skip_existing_configs? ?-overwrite_queue?
# Options:
#   -skip_existing_configs  - Don't error if config exists, skip it
#   -overwrite_queue        - If queue exists, delete and recreate
proc ::ess_queues::queue_import {json args} {
    variable db
    
    # Parse options
    set skip_existing_configs 0
    set overwrite_queue 0
    foreach arg $args {
        switch -- $arg {
            -skip_existing_configs { set skip_existing_configs 1 }
            -overwrite_queue { set overwrite_queue 1 }
        }
    }
    
    # Parse JSON
    set data [json_to_dict $json]
    
    if {![dict exists $data queue]} {
        error "Invalid queue export: missing 'queue' key"
    }
    
    set queue_def [dict get $data queue]
    set queue_name [dict get $queue_def name]
    
    # Import bundled configs first (if present)
    if {[dict exists $data configs]} {
        set configs [dict get $data configs]
        foreach config $configs {
            set config_name [dict get $config name]
            if {[::ess::configs::exists $config_name]} {
                if {$skip_existing_configs} {
                    log info "Skipping existing config: $config_name"
                    continue
                } else {
                    error "Config already exists: $config_name"
                }
            }
            
            # Re-export as JSON for import
            set config_json [dict_to_json_deep $config]
            ::ess::configs::import $config_json
            log info "Imported config: $config_name"
        }
    }
    
    # Check if queue exists
    set exists 0
    if {[catch {get_queue_id $queue_name}] == 0} {
        set exists 1
    }
    
    if {$exists} {
        if {$overwrite_queue} {
            queue_delete $queue_name
            log info "Deleted existing queue: $queue_name"
        } else {
            error "Queue already exists: $queue_name (use -overwrite_queue to replace)"
        }
    }
    
    # Create the queue
    queue_create $queue_name \
        -description [expr {[dict exists $queue_def description] ? [dict get $queue_def description] : ""}] \
        -auto_start [expr {[dict exists $queue_def auto_start] ? [dict get $queue_def auto_start] : 1}] \
        -auto_advance [expr {[dict exists $queue_def auto_advance] ? [dict get $queue_def auto_advance] : 1}] \
        -auto_datafile [expr {[dict exists $queue_def auto_datafile] ? [dict get $queue_def auto_datafile] : 1}] \
        -datafile_template [expr {[dict exists $queue_def datafile_template] ? [dict get $queue_def datafile_template] : "{suggest}"}]
    
    # Add items
    if {[dict exists $queue_def items]} {
        foreach item [dict get $queue_def items] {
            set config_name [dict get $item config_name]
            set repeat_count [expr {[dict exists $item repeat_count] ? [dict get $item repeat_count] : 1}]
            set pause_after [expr {[dict exists $item pause_after] ? [dict get $item pause_after] : 0}]
            set notes [expr {[dict exists $item notes] ? [dict get $item notes] : ""}]
            
            queue_add $queue_name $config_name \
                -repeat $repeat_count \
                -pause_after $pause_after \
                -notes $notes
        }
    }
    
    log info "Imported queue: $queue_name"
    publish_list
    
    return $queue_name
}

# Deep dict to JSON (handles nested dicts)
proc ::ess_queues::dict_to_json_deep {d} {
    package require yajltcl
    set obj [yajl create #auto]
    
    dict_to_yajl $obj $d
    
    set result [$obj get]
    $obj delete
    return $result
}

proc ::ess_queues::dict_to_yajl {obj d} {
    $obj map_open
    dict for {k v} $d {
        $obj string $k
        if {[string is integer -strict $v]} {
            $obj number $v
        } elseif {[string is double -strict $v]} {
            $obj number $v
        } elseif {$v eq "true" || $v eq "false"} {
            $obj bool [expr {$v eq "true"}]
        } elseif {[llength $v] > 1 && [catch {dict size $v}] == 0} {
            # Looks like a dict
            dict_to_yajl $obj $v
        } elseif {[llength $v] > 1} {
            # Looks like a list
            $obj array_open
            foreach item $v {
                if {[catch {dict size $item}] == 0 && [llength $item] > 1} {
                    dict_to_yajl $obj $item
                } elseif {[string is integer -strict $item]} {
                    $obj number $item
                } else {
                    $obj string $item
                }
            }
            $obj array_close
        } else {
            $obj string $v
        }
    }
    $obj map_close
}

# =============================================================================
# Helper Procedures
# =============================================================================

proc ::ess_queues::get_queue_id {name} {
    variable db
    
    set id [$db onecolumn {SELECT id FROM queues WHERE name = :name}]
    if {$id eq ""} {
        error "Queue not found: $name"
    }
    return $id
}

proc ::ess_queues::renumber_items {queue_id} {
    variable db
    
    # Get all items ordered by current position
    set items [$db eval {
        SELECT id FROM queue_items WHERE queue_id = :queue_id ORDER BY position
    }]
    
    # Renumber sequentially
    set pos 0
    foreach item_id $items {
        $db eval {UPDATE queue_items SET position = :pos WHERE id = :item_id}
        incr pos
    }
}

# JSON helpers (simple implementations - may need to match ess_configs style)
proc ::ess_queues::dict_to_json {d} {
    set pairs [list]
    dict for {k v} $d {
        if {[string is integer -strict $v] || [string is double -strict $v]} {
            lappend pairs "\"$k\":$v"
        } elseif {$v eq "true" || $v eq "false"} {
            lappend pairs "\"$k\":$v"
        } else {
            lappend pairs "\"$k\":\"[string map {\" \\\" \\ \\\\ \n \\n \r \\r \t \\t} $v]\""
        }
    }
    return "\{[join $pairs ,]\}"
}

proc ::ess_queues::list_to_json {lst} {
    set items [list]
    foreach item $lst {
        if {[llength $item] > 1 || [string index $item 0] eq "\{"} {
            # Assume it's a dict
            lappend items [dict_to_json $item]
        } else {
            lappend items "\"[string map {\" \\\" \\ \\\\} $item]\""
        }
    }
    return "\[[join $items ,]\]"
}
