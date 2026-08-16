# -*- mode: tcl -*-
#
# ess_registry-1.0.tm - Registry connection config + subject roster
#
# Provides the registry connection configuration (sole publisher of the
# ess/registry/* datapoints), the HTTP/JSON helpers other modules build
# on (ess_registry_configs), and the workgroup subject roster.
#
# The per-script status/pull/push/sandbox API that used to live here was
# retired 2026-08 — script sync belongs to the scripts subprocess
# (ess_scripts-1.0.tm + ess_sync-1.0.tm).
#
# Usage:
#   package require ess_registry
#
#   # Configure (usually done once at startup)
#   ess::registry::configure -url "https://dserv.net" -workgroup "mylab"
#
#   ess::registry::subject_names
#   ess::registry::add_subject "subject01"
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

    proc api_delete {path args} {
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

        set response [https_delete $url -timeout $timeout]
        # DELETE endpoints commonly return an empty body on success; don't
        # feed that to the JSON decoder.
        if {$response eq ""} {
            return {}
        }
        return [json_decode $response]
    }

    #=========================================================================
    # Subject Operations (workgroup roster; registry is the source of truth)
    #=========================================================================

    # Names of the subjects in the configured workgroup (active only).
    # Returns {} when no workgroup is configured or the registry is
    # unreachable -- callers treat that as "keep the current list".
    proc subject_names {} {
        variable config
        if {$config(workgroup) eq ""} { return {} }
        if {[catch {api_get "/subjects?workgroup=$config(workgroup)"} subs]} {
            return {}
        }
        return [lmap s $subs {dict get $s name}]
    }

    proc add_subject {name} {
        variable config
        set name [::ess::canonical_subject_name $name]
        if {$config(workgroup) eq ""} {
            error "Registry workgroup not configured"
        }
        api_post "/subjects?workgroup=$config(workgroup)" \
            [dict create name $name active true]
        return $name
    }

    proc remove_subject {name} {
        variable config
        set name [::ess::canonical_subject_name $name]
        if {$config(workgroup) eq ""} {
            error "Registry workgroup not configured"
        }
        api_delete "/subject/$config(workgroup)/$name"
        return $name
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
    
}

# Initialize from dserv on load
catch { ess::registry::init_from_dserv }
