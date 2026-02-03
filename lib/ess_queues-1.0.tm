# ess_queues-1_0.tm -- Queue management and run orchestration for ESS
#
# Queues are ordered sequences of configs for unattended/batch execution.
# Each queue belongs to exactly one project.
#
# Key simplifications from previous version:
#   - Queues no longer have datafile_template (now in configs)
#   - Queue items reference configs by ID (not name)
#   - No subject_override (edit config or clone it instead)
#
# Part of the configs subprocess - shares database with ess_configs.
#

package provide ess_queues 1.0

namespace eval ::ess_queues {
    # Database handle (shared with ess_configs)
    variable db ""
    
    # Current queue state
    variable state
    array set state {
        status          idle
        queue_id        0
        queue_name      ""
        project_name    ""
        position        0
        total_items     0
        current_config_id 0
        current_config_name ""
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
    #   ready        - config loaded, waiting to start
    #   running      - ESS is running
    #   flushing     - waiting for datapoints to flush
    #   paused       - manually paused
    #   between_runs - between repeats or items
    #   finished     - queue complete
    
    variable flush_delay_ms 750
}

# =============================================================================
# Initialization
# =============================================================================

proc ::ess_queues::init {db_handle} {
    variable db
    set db $db_handle
    
    # Tables are created by ess_configs now (unified schema)
    publish_state
    
    return 1
}

# =============================================================================
# Queue CRUD Operations
# =============================================================================

proc ::ess_queues::queue_create {name args} {
    variable db
    
    # Get active project from configs module
    set project [::ess::configs::project_active]
    
    set description ""
    set auto_start 1
    set auto_advance 1
    set auto_datafile 1
    set created_by ""
    
    foreach {opt val} $args {
        switch -- $opt {
            -project { set project $val }
            -description { set description $val }
            -auto_start { set auto_start [expr {$val ? 1 : 0}] }
            -auto_advance { set auto_advance [expr {$val ? 1 : 0}] }
            -auto_datafile { set auto_datafile [expr {$val ? 1 : 0}] }
            -created_by { set created_by $val }
        }
    }
    
    if {$project eq ""} {
        error "No project specified and no active project"
    }
    
    set project_id [::ess::configs::project_get_id $project]
    
    if {![regexp {^[\w\-\.]+$} $name]} {
        error "Invalid queue name: use only letters, numbers, underscores, dashes, dots"
    }
    
    # Check for duplicate in this project
    if {[$db exists {SELECT 1 FROM queues WHERE project_id = :project_id AND name = :name}]} {
        error "Queue already exists in project: $name"
    }
    
    $db eval {
        INSERT INTO queues (project_id, name, description, auto_start, auto_advance, 
                           auto_datafile, created_by)
        VALUES (:project_id, :name, :description, :auto_start, :auto_advance,
                :auto_datafile, :created_by)
    }
    
    log info "Created queue: $name in project $project"
    publish_list
    
    return $name
}

proc ::ess_queues::queue_delete {name args} {
    variable db
    variable state
    
    set project [::ess::configs::project_active]
    foreach {k v} $args {
        if {$k eq "-project"} { set project $v }
    }
    
    set queue_id [get_queue_id $name $project]
    
    # Can't delete active queue
    if {$state(status) ne "idle" && $state(queue_id) == $queue_id} {
        error "Cannot delete active queue"
    }
    
    # Items deleted via CASCADE
    $db eval {DELETE FROM queues WHERE id = :queue_id}
    
    log info "Deleted queue: $name"
    publish_list
    return $name
}

proc ::ess_queues::queue_list {args} {
    variable db
    
    set project [::ess::configs::project_active]
    set all 0
    
    foreach {k v} $args {
        switch -- $k {
            -project { set project $v }
            -all { set all $v }
        }
    }
    
    set where_clause "1=1"
    if {!$all && $project ne ""} {
        set project_id [::ess::configs::project_get_id $project]
        set where_clause "q.project_id = :project_id"
    }
    
    set result [list]
    $db eval "
        SELECT q.id, q.name, q.description, q.auto_start, q.auto_advance,
               q.auto_datafile, q.created_at, q.created_by,
               p.name as project_name,
               (SELECT COUNT(*) FROM queue_items WHERE queue_id = q.id) as item_count
        FROM queues q
        JOIN projects p ON p.id = q.project_id
        WHERE $where_clause
        ORDER BY p.name, q.name
    " row {
        lappend result [dict create \
            id $row(id) \
            name $row(name) \
            description $row(description) \
            project $row(project_name) \
            auto_start $row(auto_start) \
            auto_advance $row(auto_advance) \
            auto_datafile $row(auto_datafile) \
            item_count $row(item_count) \
            created_at $row(created_at) \
            created_by $row(created_by)]
    }
    
    return $result
}

proc ::ess_queues::queue_get {name args} {
    variable db
    
    set project [::ess::configs::project_active]
    foreach {k v} $args {
        if {$k eq "-project"} { set project $v }
    }
    
    set queue_id [get_queue_id $name $project]
    
    set result ""
    $db eval {
        SELECT q.id, q.name, q.description, q.auto_start, q.auto_advance,
               q.auto_datafile, q.created_at, q.created_by, q.project_id,
               p.name as project_name
        FROM queues q
        JOIN projects p ON p.id = q.project_id
        WHERE q.id = :queue_id
    } row {
        # Get items with config names
        set items [list]
        $db eval {
            SELECT qi.config_id, qi.position, qi.repeat_count, qi.pause_after, qi.notes,
                   c.name as config_name
            FROM queue_items qi
            JOIN configs c ON c.id = qi.config_id
            WHERE qi.queue_id = :queue_id
            ORDER BY qi.position
        } item {
            lappend items [dict create \
                config_id $item(config_id) \
                config_name $item(config_name) \
                position $item(position) \
                repeat_count $item(repeat_count) \
                pause_after $item(pause_after) \
                notes $item(notes)]
        }
        
        set result [dict create \
            id $row(id) \
            name $row(name) \
            description $row(description) \
            project $row(project_name) \
            auto_start $row(auto_start) \
            auto_advance $row(auto_advance) \
            auto_datafile $row(auto_datafile) \
            items $items \
            created_at $row(created_at) \
            created_by $row(created_by)]
    }
    
    return $result
}

proc ::ess_queues::queue_get_json {name args} {
    variable db
    
    set project [::ess::configs::project_active]
    foreach {k v} $args {
        if {$k eq "-project"} { set project $v }
    }
    
    set queue_id [get_queue_id $name $project]
    
    package require yajltcl
    set obj [yajl create #auto]
    
    $db eval {
        SELECT q.id, q.name, q.description, q.auto_start, q.auto_advance,
               q.auto_datafile, q.created_at, q.created_by,
               p.name as project_name
        FROM queues q
        JOIN projects p ON p.id = q.project_id
        WHERE q.id = :queue_id
    } row {
        $obj map_open
        $obj string "id" number $row(id)
        $obj string "name" string $row(name)
        $obj string "description" string $row(description)
        $obj string "project" string $row(project_name)
        $obj string "auto_start" bool $row(auto_start)
        $obj string "auto_advance" bool $row(auto_advance)
        $obj string "auto_datafile" bool $row(auto_datafile)
        $obj string "created_at" number $row(created_at)
        $obj string "created_by" string $row(created_by)
        
        # Build items array properly
        $obj string "items" array_open
        $db eval {
            SELECT qi.config_id, qi.position, qi.repeat_count, qi.pause_after, qi.notes,
                   c.name as config_name
            FROM queue_items qi
            JOIN configs c ON c.id = qi.config_id
            WHERE qi.queue_id = :queue_id
            ORDER BY qi.position
        } item {
            $obj map_open
            $obj string "config_id" number $item(config_id)
            $obj string "config_name" string $item(config_name)
            $obj string "position" number $item(position)
            $obj string "repeat_count" number $item(repeat_count)
            $obj string "pause_after" number $item(pause_after)
            $obj string "notes" string $item(notes)
            $obj map_close
        }
        $obj array_close
        
        $obj map_close
    }
    
    set result [$obj get]
    $obj delete
    
    if {$result eq ""} {
        return "{}"
    }
    return $result
}

proc ::ess_queues::queue_update {name args} {
    variable db
    
    set project [::ess::configs::project_active]
    set updates {}
    
    foreach {opt val} $args {
        switch -- $opt {
            -project { set project $val }
            -description {
                $db eval {UPDATE queues SET description = :val, updated_at = strftime('%s','now') WHERE id = :queue_id}
            }
            -auto_start {
                set val [expr {$val ? 1 : 0}]
                lappend updates "auto_start = $val"
            }
            -auto_advance {
                set val [expr {$val ? 1 : 0}]
                lappend updates "auto_advance = $val"
            }
            -auto_datafile {
                set val [expr {$val ? 1 : 0}]
                lappend updates "auto_datafile = $val"
            }
            -name {
                if {![regexp {^[\w\-\.]+$} $val]} {
                    error "Invalid queue name"
                }
                lappend updates "name = '$val'"
                set name $val
            }
        }
    }
    
    if {[llength $updates] > 0} {
        set queue_id [get_queue_id $name $project]
        set sql "UPDATE queues SET [join $updates ", "], updated_at = strftime('%s','now') WHERE id = $queue_id"
        $db eval $sql
    }
    
    publish_list
    return $name
}

# =============================================================================
# Queue Item Management
# =============================================================================

proc ::ess_queues::queue_add_item {queue_name config_name args} {
    variable db
    
    set project [::ess::configs::project_active]
    set position -1
    set repeat_count 1
    set pause_after 0
    set notes ""
    
    foreach {opt val} $args {
        switch -- $opt {
            -project { set project $val }
            -position { set position $val }
            -repeat { set repeat_count [expr {int($val)}] }
            -pause_after { set pause_after [expr {int($val)}] }
            -notes { set notes $val }
        }
    }
    
    set queue_id [get_queue_id $queue_name $project]
    
    # Get config ID (must be in same project)
    set config [::ess::configs::get $config_name -project $project]
    if {$config eq ""} {
        error "Config not found in project: $config_name"
    }
    set config_id [dict get $config id]
    
    # Get max position if not specified
    if {$position < 0} {
        set max_pos [$db onecolumn {
            SELECT COALESCE(MAX(position), -1) FROM queue_items WHERE queue_id = :queue_id
        }]
        set position [expr {$max_pos + 1}]
    } else {
        # Shift existing items
        $db eval {
            UPDATE queue_items 
            SET position = position + 1 
            WHERE queue_id = :queue_id AND position >= :position
        }
    }
    
    $db eval {
        INSERT INTO queue_items (queue_id, config_id, position, repeat_count, pause_after, notes)
        VALUES (:queue_id, :config_id, :position, :repeat_count, :pause_after, :notes)
    }
    
    publish_queue_items $queue_name $project
    return $position
}

proc ::ess_queues::queue_remove_item {queue_name position args} {
    variable db
    
    set project [::ess::configs::project_active]
    foreach {k v} $args {
        if {$k eq "-project"} { set project $v }
    }
    
    set queue_id [get_queue_id $queue_name $project]
    set position [expr {int($position)}]
    
    $db eval {
        DELETE FROM queue_items WHERE queue_id = :queue_id AND position = :position
    }
    
    renumber_items $queue_id
    publish_queue_items $queue_name $project
}

proc ::ess_queues::queue_clear_items {queue_name args} {
    variable db
    
    set project [::ess::configs::project_active]
    foreach {k v} $args {
        if {$k eq "-project"} { set project $v }
    }
    
    set queue_id [get_queue_id $queue_name $project]
    
    $db eval {DELETE FROM queue_items WHERE queue_id = :queue_id}
    
    publish_queue_items $queue_name $project
}

proc ::ess_queues::queue_update_item {queue_name position args} {
    variable db
    
    set project [::ess::configs::project_active]
    set repeat_count ""
    set pause_after ""
    set notes ""
    set has_notes 0
    
    foreach {opt val} $args {
        switch -- $opt {
            -project { set project $val }
            -repeat { set repeat_count [expr {int($val)}] }
            -repeat_count { set repeat_count [expr {int($val)}] }
            -pause_after { set pause_after [expr {int($val)}] }
            -notes { set notes $val; set has_notes 1 }
        }
    }
    
    set queue_id [get_queue_id $queue_name $project]
    set position [expr {int($position)}]
    
    # Build update list
    set updates {}
    if {$repeat_count ne ""} {
        lappend updates "repeat_count = :repeat_count"
    }
    if {$pause_after ne ""} {
        lappend updates "pause_after = :pause_after"
    }
    if {$has_notes} {
        lappend updates "notes = :notes"
    }
    
    if {[llength $updates] == 0} {
        error "No updates specified"
    }
    
    set sql "UPDATE queue_items SET [join $updates ", "] WHERE queue_id = :queue_id AND position = :position"
    $db eval $sql
    
    publish_queue_items $queue_name $project
    return $position
}

# =============================================================================
# Single Config Run (no queue needed)
# =============================================================================

proc ::ess_queues::run_single {config_name args} {
    variable state
    
    # Stop any active queue
    if {$state(status) ne "idle"} {
        queue_stop
    }
    
    # Get the config
    set config [::ess::configs::get $config_name]
    if {$config eq ""} {
        error "Config not found: $config_name"
    }
    
    set config_id [dict get $config id]
    set project_name [dict get $config project_name]
    
    # Initialize state for single run (no queue, just one config)
    set state(status) loading
    set state(queue_id) 0
    set state(queue_name) "(single)"
    set state(project_name) $project_name
    set state(position) 0
    set state(total_items) 1
    set state(current_config_id) $config_id
    set state(current_config_name) $config_name
    set state(current_repeat_count) 1
    set state(current_pause_after) 0
    set state(run_count) 0
    set state(global_run) 0
    set state(pause_until) 0
    set state(run_started) 0
    set state(datafile_open) 0
    set state(auto_start) 1
    set state(auto_advance) 0
    set state(auto_datafile) 1
    
    log info "Running single config: $config_name"
    publish_state
    
    # Load the config
    if {[catch {::ess::configs::load $config_id} err]} {
        log error "Failed to load config: $err"
        set state(status) idle
        publish_state
        error "Failed to load config: $err"
    }
    
    set state(status) ready
    publish_state
    
    # Start immediately
    start_run
    
    return $config_name
}

# =============================================================================
# Queue Run Orchestration
# =============================================================================

proc ::ess_queues::queue_start {queue_name args} {
    variable state
    variable db
    
    if {$state(status) ni {idle finished}} {
        error "Queue already active: $state(queue_name)"
    }
    
    set project [::ess::configs::project_active]
    set start_position 0
    
    foreach {opt val} $args {
        switch -- $opt {
            -project { set project $val }
            -position { set start_position [expr {int($val)}] }
        }
    }
    
    set queue_id [get_queue_id $queue_name $project]
    
    # Get queue info
    set queue_info [$db eval {
        SELECT auto_start, auto_advance, auto_datafile FROM queues WHERE id = :queue_id
    }]
    lassign $queue_info auto_start auto_advance auto_datafile
    
    set item_count [$db onecolumn {
        SELECT COUNT(*) FROM queue_items WHERE queue_id = :queue_id
    }]
    
    if {$item_count == 0} {
        error "Queue is empty: $queue_name"
    }
    
    # Initialize state
    set state(status) loading
    set state(queue_id) $queue_id
    set state(queue_name) $queue_name
    set state(project_name) $project
    set state(position) $start_position
    set state(total_items) $item_count
    set state(run_count) 0
    set state(global_run) 0
    set state(current_config_id) 0
    set state(current_config_name) ""
    set state(pause_until) 0
    set state(run_started) 0
    set state(datafile_open) 0
    set state(auto_start) $auto_start
    set state(auto_advance) $auto_advance
    set state(auto_datafile) $auto_datafile
    
    log info "Starting queue: $queue_name (items: $item_count, position: $start_position)"
    publish_state
    
    load_current_item
    
    return $queue_name
}

proc ::ess_queues::queue_stop {} {
    variable state
    
    if {$state(status) eq "idle"} {
        return
    }
    
    log info "Stopping queue: $state(queue_name)"
    
    if {$state(status) eq "running"} {
        ess_stop
    }
    
    close_datafile
    
    # Reset state
    array set state {
        status          idle
        queue_id        0
        queue_name      ""
        project_name    ""
        position        0
        total_items     0
        current_config_id 0
        current_config_name ""
        run_count       0
        pause_until     0
        run_started     0
    }
    
    publish_state
}

proc ::ess_queues::queue_pause {} {
    variable state
    
    if {$state(status) ni {running ready}} {
        error "Cannot pause: queue is $state(status)"
    }
    
    log info "Pausing queue at position $state(position)"
    
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
    
    log info "Resuming queue"
    
    set state(status) ready
    publish_state
    
    if {$state(auto_start)} {
        start_run
    }
}

proc ::ess_queues::queue_skip {} {
    variable state
    
    if {$state(status) eq "idle"} {
        error "No active queue"
    }
    
    log info "Skipping to next item"
    
    if {$state(status) eq "running"} {
        ess_stop
        close_datafile
    }
    
    set state(run_count) 0
    advance_position
}

proc ::ess_queues::queue_next {} {
    variable state
    
    if {$state(status) eq "idle"} {
        error "No active queue"
    }
    
    if {$state(status) eq "running"} {
        ess_stop
        close_datafile
    }
    
    set state(run_started) 0
    
    if {$state(run_count) < $state(current_repeat_count)} {
        set state(status) ready
        publish_state
        if {$state(auto_start)} {
            start_run
        }
    } else {
        advance_position
    }
}

proc ::ess_queues::queue_reset {} {
    variable state
    
    if {$state(status) eq "idle"} {
        # Nothing to reset
        return
    }
    
    log info "Resetting queue: $state(queue_name) to position 0"
    
    # Stop ESS if running
    if {$state(status) eq "running"} {
        ess_stop
    }
    
    # Close any open datafile
    close_datafile
    
    # Reset to beginning but keep queue active
    set state(position) 0
    set state(run_count) 0
    set state(current_config_id) 0
    set state(current_config_name) ""
    set state(pause_until) 0
    set state(run_started) 0
    set state(status) idle
    
    publish_state
}

proc ::ess_queues::run_close {} {
    variable state
    
    if {$state(status) eq "idle"} {
        error "No active run to close"
    }
    
    if {$state(status) eq "running"} {
        # Signal run complete - this triggers the normal state machine flow
        # (flushing, datafile close, advance/idle)
        dservSet ess/run_state complete
        return
    }
    
    log info "Closing run: $state(current_config_name) (position $state(position))"
    
    # Close any open datafile
    close_datafile
    
    # For single config runs, go to idle
    if {$state(queue_name) eq "(single)"} {
        set state(status) idle
        set state(current_config_name) ""
        publish_state
        return
    }
    
    # For queue runs, check if more repeats needed
    if {$state(run_count) < $state(current_repeat_count)} {
        # More repeats of this config - stay ready
        set state(status) ready
        publish_state
        if {$state(auto_start)} {
            start_run
        }
        return
    }
    
    # Check if there's pause_after time
    if {$state(current_pause_after) > 0} {
        set state(status) between_runs
        set state(pause_until) [expr {[clock seconds] + $state(current_pause_after)}]
        publish_state
        return
    }
    
    # Advance to next position if auto_advance, otherwise go between_runs
    if {$state(auto_advance)} {
        advance_position
    } else {
        set state(status) between_runs
        publish_state
    }
}

# =============================================================================
# Internal Orchestration
# =============================================================================

proc ::ess_queues::load_current_item {} {
    variable state
    variable db
    
    # Get item at current position
    set item [$db eval {
        SELECT qi.config_id, qi.repeat_count, qi.pause_after, c.name as config_name
        FROM queue_items qi
        JOIN configs c ON c.id = qi.config_id
        WHERE qi.queue_id = :state(queue_id) AND qi.position = :state(position)
    }]
    
    if {$item eq ""} {
        log info "No more items - queue finished"
        set state(status) finished
        publish_state
        return
    }
    
    lassign $item config_id repeat_count pause_after config_name
    
    set state(current_config_id) $config_id
    set state(current_config_name) $config_name
    set state(current_repeat_count) $repeat_count
    set state(current_pause_after) $pause_after
    set state(run_count) 0
    
    log info "Loading config: $config_name (repeats: $repeat_count)"
    
    # Load the config
    if {[catch {::ess::configs::load $config_id} err]} {
        log error "Failed to load config: $err"
        set state(status) paused
        publish_state
        return
    }
    
    set state(status) ready
    publish_state
    
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
    
    log info "Starting run $state(run_count)/$state(current_repeat_count) (global: $state(global_run))"
    
    # Open datafile if auto_datafile enabled
    if {$state(auto_datafile)} {
        if {![open_datafile]} {
            set state(status) paused
            publish_state
            return
        }
    }
    
    # Start ESS
    set state(status) running
    set state(run_started) [clock seconds]
    publish_state
    
    ess_start
}

proc ::ess_queues::open_datafile {} {
    variable state
    
    if {$state(datafile_open)} {
        close_datafile
    }
    
    # Get config for file template
    set config [::ess::configs::get $state(current_config_id)]
    if {$config eq ""} {
        log error "Config not found: $state(current_config_id)"
        return 0
    }
    
    # Generate basename using config's template (with queue context)
    set basename [generate_file_basename_queue $config]
    
    log info "Opening datafile: $basename"
    
    if {[catch {send ess "::ess::file_open {$basename}"} result]} {
        log error "Failed to open datafile: $result"
        return 0
    }
    
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

proc ::ess_queues::generate_file_basename_queue {config} {
    variable state
    
    set template [dict get $config file_template]
    
    # If empty, use ESS default with run suffix
    if {$template eq ""} {
        set basename [send ess "::ess::file_suggest"]
        if {$state(current_repeat_count) > 1} {
            append basename "_r$state(run_count)"
        }
        return $basename
    }
    
    # Get values from config
    set subject [dict get $config subject]
    if {$subject eq ""} { set subject "unknown" }
    
    set system [dict get $config system]
    set protocol [dict get $config protocol]
    set variant [dict get $config variant]
    set config_name [dict get $config name]
    set project_name [dict get $config project_name]
    
    # Timestamps
    set now [clock seconds]
    set date [clock format $now -format "%Y%m%d"]
    set date_short [clock format $now -format "%y%m%d"]
    set time [clock format $now -format "%H%M%S"]
    set time_short [clock format $now -format "%H%M"]
    
    # Clean for filename
    set config_clean [regsub -all {[^a-zA-Z0-9_-]} $config_name "_"]
    set subject_clean [regsub -all {[^a-zA-Z0-9_-]} $subject "_"]
    set project_clean [regsub -all {[^a-zA-Z0-9_-]} $project_name "_"]
    set queue_clean [regsub -all {[^a-zA-Z0-9_-]} $state(queue_name) "_"]
    
    # Base substitutions
    set basename $template
    
    # Handle numeric variables with format specifiers
    foreach {var value} [list position $state(position) run $state(run_count) global $state(global_run)] {
        # Handle {var:NN} format
        while {[regexp "\\{${var}:(\\d+)\\}" $basename -> width]} {
            set formatted [format "%0${width}d" $value]
            regsub "\\{${var}:\\d+\\}" $basename $formatted basename
        }
        # Handle plain {var}
        set basename [string map [list "\{$var\}" $value] $basename]
    }
    
    # String substitutions
    set subst_map [list \
        "{subject}"    $subject_clean \
        "{system}"     $system \
        "{protocol}"   $protocol \
        "{variant}"    $variant \
        "{config}"     $config_clean \
        "{project}"    $project_clean \
        "{queue}"      $queue_clean \
        "{date}"       $date \
        "{date_short}" $date_short \
        "{time}"       $time \
        "{time_short}" $time_short \
        "{timestamp}"  $now]
    
    return [string map $subst_map $basename]
}

proc ::ess_queues::close_datafile {} {
    variable state
    
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

proc ::ess_queues::advance_position {} {
    variable state
    
    incr state(position)
    
    if {$state(position) >= $state(total_items)} {
        log info "Queue complete"
        set state(status) finished
        publish_state
        return
    }
    
    set state(status) loading
    publish_state
    
    load_current_item
}

proc ::ess_queues::on_ess_run_state {run_state} {
    variable state
    variable flush_delay_ms

    if {$state(status) ne "running"} {
        return
    }
    
    if {$run_state eq "complete"} {
        log info "Run complete"
        
        # Start flush delay
        set state(status) flushing
        set state(flush_until) [expr {[clock milliseconds] + $flush_delay_ms}]
        publish_state
    }
}

proc ::ess_queues::tick {} {
    variable state
    
    switch $state(status) {
        flushing {
            if {[clock milliseconds] >= $state(flush_until)} {
                close_datafile
                
                if {$state(current_pause_after) > 0 && 
                    $state(run_count) >= $state(current_repeat_count)} {
                    set state(status) between_runs
                    set state(pause_until) [expr {[clock seconds] + $state(current_pause_after)}]
                } elseif {$state(run_count) < $state(current_repeat_count)} {
                    set state(status) ready
                    if {$state(auto_start)} {
                        start_run
                    }
} elseif {$state(auto_advance)} {
                    advance_position
                } elseif {$state(queue_name) eq "(single)"} {
                    # Single config run complete - go idle
                    set state(status) idle
                    set state(current_config_name) ""
                } else {
                    set state(status) between_runs
                }		    
                publish_state
            }
        }
        between_runs {
            if {$state(pause_until) > 0 && [clock seconds] >= $state(pause_until)} {
                set state(pause_until) 0
                if {$state(auto_advance)} {
                    advance_position
                }
            }
        }
    }
}

# =============================================================================
# ESS Control
# =============================================================================

proc ::ess_queues::ess_start {} {
    log info "Starting ESS"
    if {[catch {send ess "ess::start"} err]} {
        log error "Failed to start ESS: $err"
        return 0
    }
    return 1
}

proc ::ess_queues::ess_stop {} {
    log info "Stopping ESS"
    if {[catch {send ess "ess::stop"} err]} {
        log warning "Error stopping ESS: $err"
    }
    return 1
}

# =============================================================================
# Publishing
# =============================================================================

proc ::ess_queues::publish_state {} {
    variable state
    
    # Individual datapoints (legacy)
    dservSet queues/status $state(status)
    dservSet queues/active $state(queue_name)
    dservSet queues/project $state(project_name)
    dservSet queues/position $state(position)
    dservSet queues/total $state(total_items)
    dservSet queues/current_config $state(current_config_name)
    dservSet queues/run_count $state(run_count)
    dservSet queues/repeat_count $state(current_repeat_count)
    dservSet queues/global_run $state(global_run)
    
    # Combined JSON for JS clients
    set json "{\"status\":\"$state(status)\",\"queue_name\":\"$state(queue_name)\",\"project_name\":\"$state(project_name)\",\"position\":$state(position),\"total_items\":$state(total_items),\"current_config\":\"$state(current_config_name)\",\"run_count\":$state(run_count),\"repeat_total\":$state(current_repeat_count),\"auto_start\":$state(auto_start),\"auto_advance\":$state(auto_advance)}"
    
    dservSet queues/state $json
}

proc ::ess_queues::publish_list {} {
    variable db
    
    set project [::ess::configs::project_active]
    
    package require yajltcl
    set obj [yajl create #auto]
    $obj array_open
    
    set where "1=1"
    if {$project ne ""} {
        set project_id [::ess::configs::project_get_id $project]
        set where "q.project_id = $project_id"
    }
    
    $db eval "
        SELECT q.id, q.name, q.description, q.auto_start, q.auto_advance,
               q.auto_datafile, p.name as project_name,
               (SELECT COUNT(*) FROM queue_items WHERE queue_id = q.id) as item_count
        FROM queues q
        JOIN projects p ON p.id = q.project_id
        WHERE $where
        ORDER BY q.name
    " row {
        $obj map_open
        $obj string "id" number $row(id)
        $obj string "name" string $row(name)
        $obj string "description" string $row(description)
        $obj string "project" string $row(project_name)
        $obj string "auto_start" bool $row(auto_start)
        $obj string "auto_advance" bool $row(auto_advance)
        $obj string "auto_datafile" bool $row(auto_datafile)
        $obj string "item_count" number $row(item_count)
        $obj map_close
    }
    
    $obj array_close
    set result [$obj get]
    $obj delete
    
    dservSet queues/list $result
}

proc ::ess_queues::publish_queue_items {queue_name project} {
    variable db
    
    set queue_id [get_queue_id $queue_name $project]
    
    package require yajltcl
    set obj [yajl create #auto]
    $obj array_open
    
    $db eval {
        SELECT qi.position, qi.config_id, qi.repeat_count, qi.pause_after, qi.notes,
               c.name as config_name
        FROM queue_items qi
        JOIN configs c ON c.id = qi.config_id
        WHERE qi.queue_id = :queue_id
        ORDER BY qi.position
    } row {
        $obj map_open
        $obj string "position" number $row(position)
        $obj string "config_id" number $row(config_id)
        $obj string "config_name" string $row(config_name)
        $obj string "repeat_count" number $row(repeat_count)
        $obj string "pause_after" number $row(pause_after)
        $obj string "notes" string $row(notes)
        $obj map_close
    }
    
    $obj array_close
    set result [$obj get]
    $obj delete
    
    dservSet queues/items/$queue_name $result
}

# =============================================================================
# Helpers
# =============================================================================

proc ::ess_queues::get_queue_id {name project} {
    variable db
    
    if {$project eq ""} {
        error "Project required for queue lookup"
    }
    
    set project_id [::ess::configs::project_get_id $project]
    
    set id [$db onecolumn {
        SELECT id FROM queues WHERE project_id = :project_id AND name = :name
    }]
    if {$id eq ""} {
        error "Queue not found in project $project: $name"
    }
    return $id
}

proc ::ess_queues::renumber_items {queue_id} {
    variable db
    
    set items [$db eval {
        SELECT id FROM queue_items WHERE queue_id = :queue_id ORDER BY position
    }]
    
    set pos 0
    foreach item_id $items {
        $db eval {UPDATE queue_items SET position = :pos WHERE id = :item_id}
        incr pos
    }
}

proc ::ess_queues::log {level msg} {
    set timestamp [clock format [clock seconds] -format "%H:%M:%S"]
    set full_msg "\[$timestamp\] \[$level\] $msg"
    
    if {[catch {dservSet queues/log $full_msg}]} {
        puts "ess_queues $level: $msg"
    }
}
