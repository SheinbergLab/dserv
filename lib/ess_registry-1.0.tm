# -*- mode: tcl -*-
#
# ess_registry-1.0.tm - Registry client for ESS script management
#
# Provides Tcl-level integration with dserv-agent's ESS Registry API.
# Uses https_get/https_post commands from TclHttps.cpp
#
# Usage:
#   package require ess_registry
#   
#   # Configure (usually done once at startup)
#   ess::registry::configure -url "http://registry.local:8080" -workgroup "brown-sheinberg"
#   
#   # Check sync status
#   ess::registry::status variants
#   
#   # Pull from registry to local
#   ess::registry::pull variants
#   
#   # Push local to registry  
#   ess::registry::push variants -user david -comment "Fixed timing"
#   
#   # List available versions/sandboxes
#   ess::registry::versions
#   
#   # Create/manage sandboxes
#   ess::registry::sandbox create -user david
#   ess::registry::sandbox promote -from david -comment "Ready for production"
#

package provide ess_registry 1.0

namespace eval ess::registry {
    # Configuration
    variable config
    array set config {
        url         ""
        workgroup   ""
        user        "unknown"
        timeout     10000
        version     "main"
    }
    
    # Cache for avoiding repeated requests
    variable cache
    array set cache {}
    
    #=========================================================================
    # Configuration
    #=========================================================================
    
    proc configure {args} {
        variable config
        
        # Parse arguments
        foreach {opt val} $args {
            switch -exact -- $opt {
                -url       { set config(url) $val }
                -workgroup { set config(workgroup) $val }
                -user      { set config(user) $val }
                -timeout   { set config(timeout) $val }
                -version   { set config(version) $val }
                default {
                    error "Unknown option: $opt (expected -url, -workgroup, -user, -timeout, -version)"
                }
            }
        }
        
        # Also update dserv datapoints for UI visibility
        catch { dservSet ess/registry/url $config(url) }
        catch { dservSet ess/registry/workgroup $config(workgroup) }
        catch { dservSet ess/registry/user $config(user) }
        catch { dservSet ess/registry/version $config(version) }
        
        return [array get config]
    }
    
    proc cget {option} {
        variable config
        set opt [string trimleft $option -]
        if {![info exists config($opt)]} {
            error "Unknown option: $option"
        }
        return $config($opt)
    }
    
    # Initialize from dserv datapoints if available
    proc init_from_dserv {} {
        variable config
        
        catch {
            if {[dservExists ess/registry/url]} {
                set config(url) [dservGet ess/registry/url]
            }
        }
        catch {
            if {[dservExists ess/registry/workgroup]} {
                set config(workgroup) [dservGet ess/registry/workgroup]
            }
        }
        catch {
            if {[dservExists ess/registry/user]} {
                set config(user) [dservGet ess/registry/user]
            }
        }
    }
    
    #=========================================================================
    # HTTP Helpers
    #=========================================================================
    
    proc api_get {path args} {
        variable config
        
        if {$config(url) eq ""} {
            error "Registry URL not configured"
        }
        
        set url "$config(url)/api/v1/ess$path"
        set timeout $config(timeout)
        
        # Parse options
        foreach {opt val} $args {
            if {$opt eq "-timeout"} {
                set timeout $val
            }
        }
        
        # Use https_get (from TclHttps.cpp)
        set response [https_get $url -timeout $timeout]
        
        # Parse JSON response
        return [json_decode $response]
    }
    
    proc api_post {path body args} {
        variable config
        
        if {$config(url) eq ""} {
            error "Registry URL not configured"
        }
        
        set url "$config(url)/api/v1/ess$path"
        set timeout $config(timeout)
        
        foreach {opt val} $args {
            if {$opt eq "-timeout"} {
                set timeout $val
            }
        }
        
        # Encode body as JSON if it's a dict
        if {[string index $body 0] ne "\{"} {
            set body [json_encode $body]
        }
        
        set response [https_post $url $body -timeout $timeout]
        return [json_decode $response]
    }
    
    proc api_put {path body args} {
        # https_post with PUT semantics - we'll add a method parameter
        # For now, POST works for our API since we handle both
        return [api_post $path $body {*}$args]
    }
    
    #=========================================================================
    # JSON Helpers (minimal implementation)
    #=========================================================================
    
    proc json_encode {dict_or_value} {
        # Simple JSON encoder for our needs
        if {[llength $dict_or_value] == 0} {
            return "null"
        }
        
        # Check if it's a dict (even number of elements, string keys)
        if {[llength $dict_or_value] > 1 && [llength $dict_or_value] % 2 == 0} {
            set pairs {}
            dict for {k v} $dict_or_value {
                set encoded_v [json_encode_value $v]
                lappend pairs "\"$k\":$encoded_v"
            }
            return "\{[join $pairs ,]\}"
        }
        
        return [json_encode_value $dict_or_value]
    }
    
    proc json_encode_value {v} {
        # Encode a single value
        if {$v eq "true" || $v eq "false" || $v eq "null"} {
            return $v
        }
        if {[string is integer -strict $v] || [string is double -strict $v]} {
            return $v
        }
        # Escape string
        set v [string map {
            \\ \\\\
            \" \\\"
            \n \\n
            \r \\r
            \t \\t
        } $v]
        return "\"$v\""
    }
    
    proc json_decode {json} {
        # Use yajltcl if available, otherwise simple parsing
        if {[catch {package require yajltcl}]} {
            return [json_decode_simple $json]
        }
        
        # yajltcl available
        return [::yajl::json2dict $json]
    }
    
    proc json_decode_simple {json} {
        # Very simple JSON decoder for basic objects
        # Handles {"key": "value", "key2": 123} format
        
        set json [string trim $json]
        
        if {$json eq "null"} { return "" }
        if {$json eq "true"} { return 1 }
        if {$json eq "false"} { return 0 }
        
        # String
        if {[string index $json 0] eq "\""} {
            return [string range $json 1 end-1]
        }
        
        # Number
        if {[string is double -strict $json]} {
            return $json
        }
        
        # Object - basic parsing
        if {[string index $json 0] eq "\{"} {
            set result {}
            # Remove braces
            set content [string range $json 1 end-1]
            # Split on commas (naive - doesn't handle nested)
            foreach pair [split $content ,] {
                if {[regexp {"([^"]+)"\s*:\s*(.+)} $pair -> key val]} {
                    set val [string trim $val]
                    dict set result $key [json_decode_simple $val]
                }
            }
            return $result
        }
        
        # Array
        if {[string index $json 0] eq "\["} {
            set result {}
            set content [string range $json 1 end-1]
            foreach item [split $content ,] {
                lappend result [json_decode_simple [string trim $item]]
            }
            return $result
        }
        
        return $json
    }
    
    #=========================================================================
    # Script Path Helpers
    #=========================================================================
    
    proc get_current_context {} {
        # Get current system/protocol/variant from ess namespace
        set ctx [dict create]
        
        catch { dict set ctx project $::ess::current(project) }
        catch { dict set ctx system $::ess::current(system) }
        catch { dict set ctx protocol $::ess::current(protocol) }
        catch { dict set ctx variant $::ess::current(variant) }
        
        return $ctx
    }
    
    proc script_type_to_path {type} {
        variable config
        set ctx [get_current_context]
        
        set workgroup $config(workgroup)
        set system [dict get $ctx system]
        
        # Protocol is "_" for system-level scripts
        if {$type eq "system"} {
            set proto "_"
        } else {
            set proto [dict get $ctx protocol]
        }
        
        return [list $workgroup $system $proto $type]
    }
    
    proc get_local_script_content {type} {
        # Get content from local filesystem via ess namespace
        switch -exact $type {
            system   { return [::ess::system_script] }
            protocol { return [::ess::protocol_script] }
            loaders  { return [::ess::loaders_script] }
            variants { return [::ess::variants_script] }
            stim     { return [::ess::stim_script] }
            default  { error "Unknown script type: $type" }
        }
    }

    proc compute_checksum {content} {
       # Use native sha256 command (from TclSha256.cpp)
       return [sha256 $content]
    }
    
    #=========================================================================
    # Core Operations
    #=========================================================================
    
    # Check sync status between local and registry
    proc status {type {version ""}} {
        variable config
        
        if {$version eq ""} {
            set version $config(version)
        }
        
        lassign [script_type_to_path $type] wg sys proto stype
        
        # Get local content and checksum
        set local_content [get_local_script_content $type]
        set local_checksum [compute_checksum $local_content]
        
        # Get registry info
        set path "/script/$wg/$sys/$proto/$stype"
        if {$version ne "main"} {
            append path "?version=$version"
        }
        
        if {[catch {
            set remote [api_get $path]
        } err]} {
            # Script not in registry
            return [dict create \
                status "local_only" \
                local_checksum $local_checksum \
                message "Script not found in registry"]
        }
        
        set remote_checksum [dict get $remote checksum]
        
        if {$local_checksum eq $remote_checksum} {
            return [dict create \
                status "synced" \
                checksum $local_checksum \
                updated_by [dict get $remote updatedBy] \
                updated_at [dict get $remote updatedAt]]
        } else {
            return [dict create \
                status "modified" \
                local_checksum $local_checksum \
                remote_checksum $remote_checksum \
                updated_by [dict get $remote updatedBy] \
                updated_at [dict get $remote updatedAt]]
        }
    }
    
    # Pull script from registry to local filesystem
    proc pull {type args} {
        variable config
        
        # Parse options
        set version $config(version)
        set apply 0
        set validate 1
        
        foreach {opt val} $args {
            switch -exact -- $opt {
                -version  { set version $val }
                -apply    { set apply $val }
                -validate { set validate $val }
            }
        }
        
        lassign [script_type_to_path $type] wg sys proto stype
        
        # Fetch from registry
        set path "/script/$wg/$sys/$proto/$stype"
        if {$version ne "main"} {
            append path "?version=$version"
        }
        
        set remote [api_get $path]
        set content [dict get $remote content]
        
        # Validate before saving (optional)
        if {$validate} {
            set validation [::ess::validate_script $content $type "fast"]
            if {![dict get $validation valid]} {
                error "Pulled script failed validation: [dict get $validation errors]"
            }
        }
        
        # Save locally using ess::save_script
        ::ess::save_script $type $content
        
        # Optionally reload the system
        if {$apply} {
            ::ess::reload_system
        }
        
        # Update datapoint for UI
        catch { dservSet ess/registry/last_pull [clock seconds] }

        dservSet ess/registry/sync_status "synced"

        return [dict create \
            success 1 \
            type $type \
            version $version \
            checksum [dict get $remote checksum]]
    }
    
    # Push local script to registry
    proc push {type args} {
        variable config
        
        # Parse options
        set version $config(version)
        set user $config(user)
        set comment ""
        set expected_checksum ""
        
        foreach {opt val} $args {
            switch -exact -- $opt {
                -version  { set version $val }
                -user     { set user $val }
                -comment  { set comment $val }
                -expected { set expected_checksum $val }
            }
        }
        
        lassign [script_type_to_path $type] wg sys proto stype
        
        # Get local content
        set content [get_local_script_content $type]
        
        if {$content eq ""} {
            error "No local content for script type: $type"
        }
        
        # Build request body
        set body [dict create \
            content $content \
            updatedBy $user \
            comment $comment]
        
        if {$expected_checksum ne ""} {
            dict set body expectedChecksum $expected_checksum
        }
        
        # POST to registry
        set path "/script/$wg/$sys/$proto/$stype"
        if {$version ne "main"} {
            append path "?version=$version"
        }
        
        set result [api_post $path [json_encode $body]]
        
        # Update datapoint for UI
        catch { dservSet ess/registry/last_push [clock seconds] }

        dservSet ess/registry/sync_status "synced"
        
        return $result
    }
    
    #=========================================================================
    # Version/Sandbox Management
    #=========================================================================
    
    # List available versions for current system
    proc versions {} {
        variable config
        
        set ctx [get_current_context]
        set sys [dict get $ctx system]
        
        set path "/sandbox/$config(workgroup)/$sys/versions"
        return [api_get $path]
    }
    
    # Sandbox operations
    proc sandbox {action args} {
        variable config
        
        set ctx [get_current_context]
        set sys [dict get $ctx system]
        set wg $config(workgroup)
        
        switch -exact $action {
            create {
                # Create new sandbox from main (or specified version)
                set from_version "main"
                set to_version $config(user)
                set comment ""
                
                foreach {opt val} $args {
                    switch -exact -- $opt {
                        -from    { set from_version $val }
                        -to      { set to_version $val }
                        -user    { set to_version $val }
                        -comment { set comment $val }
                    }
                }
                
                set body [dict create \
                    fromVersion $from_version \
                    toVersion $to_version \
                    username $config(user) \
                    comment $comment]
                
                return [api_post "/sandbox/$wg/$sys/create" [json_encode $body]]
            }
            
            promote {
                # Promote sandbox to main
                set from_version $config(user)
                set to_version "main"
                set comment ""
                
                foreach {opt val} $args {
                    switch -exact -- $opt {
                        -from    { set from_version $val }
                        -to      { set to_version $val }
                        -comment { set comment $val }
                    }
                }
                
                set body [dict create \
                    fromVersion $from_version \
                    toVersion $to_version \
                    username $config(user) \
                    comment $comment]
                
                return [api_post "/sandbox/$wg/$sys/promote" [json_encode $body]]
            }
            
            sync {
                # Sync sandbox with main
                set from_version "main"
                set to_version $config(user)
                set conflicts "overwrite"
                
                foreach {opt val} $args {
                    switch -exact -- $opt {
                        -from      { set from_version $val }
                        -to        { set to_version $val }
                        -conflicts { set conflicts $val }
                    }
                }
                
                set body [dict create \
                    fromVersion $from_version \
                    toVersion $to_version \
                    username $config(user) \
                    conflicts $conflicts]
                
                return [api_post "/sandbox/$wg/$sys/sync" [json_encode $body]]
            }
            
            delete {
                set version ""
                foreach {opt val} $args {
                    if {$opt eq "-version"} {
                        set version $val
                    }
                }
                if {$version eq ""} {
                    set version $config(user)
                }
                if {$version eq "main"} {
                    error "Cannot delete main version"
                }
                
                # DELETE request - use POST with delete action for now
                return [api_post "/sandbox/$wg/$sys/$version" [json_encode {action delete}]]
            }
            
            default {
                error "Unknown sandbox action: $action (expected create, promote, sync, delete)"
            }
        }
    }
    
    #=========================================================================
    # Batch Operations
    #=========================================================================
    
    # Pull all scripts for current protocol
    proc pull_all {args} {
        set results {}
        foreach type {system protocol loaders variants stim} {
            if {[catch {
                dict set results $type [pull $type {*}$args]
            } err]} {
                dict set results $type [dict create error $err]
            }
        }
        return $results
    }
    
    # Push all modified scripts
    proc push_all {args} {
        set results {}
        foreach type {system protocol loaders variants stim} {
            # Check if modified before pushing
            set st [status $type]
            if {[dict get $st status] ne "synced"} {
                if {[catch {
                    dict set results $type [push $type {*}$args]
                } err]} {
                    dict set results $type [dict create error $err]
                }
            } else {
                dict set results $type [dict create skipped "already synced"]
            }
        }
        return $results
    }
    
    # Get status of all scripts
    proc status_all {{version ""}} {
        set results {}
        foreach type {system protocol loaders variants stim} {
            if {[catch {
                dict set results $type [status $type $version]
            } err]} {
                dict set results $type [dict create error $err]
            }
        }
        return $results
    }
    
    #=========================================================================
    # System Browser (for workbench)
    #=========================================================================
    
    # List all systems in workgroup
    proc list_systems {} {
        variable config
        return [api_get "/systems?workgroup=$config(workgroup)"]
    }
    
    # List templates (zoo)
    proc list_templates {} {
        return [api_get "/templates"]
    }
    
    # Get system details with all scripts
    proc get_system {name {version ""}} {
        variable config
        set path "/system/$config(workgroup)/$name"
        if {$version ne ""} {
            append path "/$version"
        }
        return [api_get $path]
    }
    
    # Add template to workgroup
    proc add_template {template_name {template_version "latest"}} {
        variable config
        
        set body [dict create \
            templateSystem $template_name \
            templateVersion $template_version \
            targetWorkgroup $config(workgroup) \
            addedBy $config(user)]
        
        return [api_post "/add-to-workgroup" [json_encode $body]]
    }
    
    #=========================================================================
    # History
    #=========================================================================
    
    proc history {type {limit 10}} {
        variable config
        lassign [script_type_to_path $type] wg sys proto stype
        
        set path "/history/$wg/$sys/$proto/$stype"
        return [api_get $path]
    }
    
    # Restore from history
    proc restore {type history_id} {
        variable config
        
        # Get history entry
        set hist [history $type]
        
        # Find the entry
        foreach entry [dict get $hist history] {
            if {[dict get $entry id] == $history_id} {
                set content [dict get $entry content]
                
                # Save locally
                ::ess::save_script $type $content
                
                return [dict create \
                    success 1 \
                    restored_from $history_id \
                    saved_by [dict get $entry savedBy] \
                    saved_at [dict get $entry savedAt]]
            }
        }
        
        error "History entry not found: $history_id"
    }
}

# Initialize from dserv on load
catch { ess::registry::init_from_dserv }
