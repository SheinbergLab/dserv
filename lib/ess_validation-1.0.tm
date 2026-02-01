# -*- mode: tcl -*-
#
# ess_validation-1.0.tm - Enhanced script validation for ESS
#
# Provides safe script validation using isolated interpreters
# to catch errors before they break the running system.
#
# Usage:
#   package require ess_validation
#   
#   # Quick syntax check
#   set result [ess::validation::check_syntax $content]
#   
#   # Full source test in isolated interp
#   set result [ess::validation::safe_source_test $type $content]
#   
#   # Validate and save (atomic operation)
#   ess::validation::validated_save $type $content -apply 1
#

package provide ess_validation 1.0
package require Tcl 8.6

namespace eval ess::validation {
    
    # Validation levels
    variable levels {
        syntax       1
        parse        2
        source_test  3
        full         4
    }
    
    #=========================================================================
    # Syntax Validation
    #=========================================================================
    
    # Quick Tcl syntax check using info complete
    proc check_syntax {content} {
        set errors {}
        set warnings {}
        set line_num 0
        
        # Check each line for basic issues
        foreach line [split $content \n] {
            incr line_num
            
            # Check for unbalanced braces/brackets
            set open_braces [regexp -all {\{} $line]
            set close_braces [regexp -all {\}} $line]
            set open_brackets [regexp -all {\[} $line]
            set close_brackets [regexp -all {\]} $line]
            
            # Track brace balance (this is simplistic - real check is below)
            
            # Check for common mistakes
            if {[regexp {^\s*proc\s+\w+\s*$} $line]} {
                lappend warnings [dict create \
                    line $line_num \
                    message "proc declaration appears incomplete (missing args and body)"]
            }
            
            if {[regexp {\$\{[^}]*$} $line]} {
                lappend warnings [dict create \
                    line $line_num \
                    message "Unclosed variable substitution"]
            }
        }
        
        # Use Tcl's own syntax checker
        if {![info complete $content]} {
            lappend errors [dict create \
                line 0 \
                message "Script has unbalanced braces, brackets, or quotes"]
        }
        
        # Try to parse without executing
        if {[catch {
            # Create a minimal interp just for parsing
            set test_interp [interp create]
            
            # Override all commands to do nothing - we just want parsing
            interp eval $test_interp {
                rename proc _orig_proc
                _orig_proc proc {name args body} {
                    # Just validate the body is parseable
                    if {![info complete $body]} {
                        error "Invalid proc body"
                    }
                }
            }
            
            # Try to parse
            interp eval $test_interp [list info complete $content]
            interp delete $test_interp
        } parse_error]} {
            lappend errors [dict create \
                line 0 \
                message "Parse error: $parse_error"]
        }
        
        set valid [expr {[llength $errors] == 0}]
        
        return [dict create \
            valid $valid \
            errors $errors \
            warnings $warnings]
    }
    
    #=========================================================================
    # Safe Source Test
    #=========================================================================
    
    # Create an isolated interpreter with ESS stubs for testing
    proc create_test_interp {type} {
        set interp [interp create -safe]
        
        # Add minimal ESS stubs based on script type
        interp eval $interp {
            # Namespace structure
            namespace eval ess {
                variable current
                array set current {
                    project ""
                    system ""
                    protocol ""
                    variant ""
                }
            }
            
            namespace eval ess::em {}
            namespace eval ess::touch {}
            
            # Stub out common ESS procedures
            proc ess::param {args} { return 1 }
            proc ess::state {name args} { return 1 }
            proc ess::transition {args} { return 1 }
            proc ess::callback {name args} { return 1 }
            proc ess::evt_put {args} { return 1 }
            proc ess::em_init {args} { return 1 }
            proc ess::touch_init {args} { return 1 }
            proc ess::sound_init {args} { return 1 }
            
            # Stub dserv commands
            proc dservSet {args} { return 1 }
            proc dservGet {name} { return "" }
            proc dservExists {name} { return 0 }
            proc dservTouch {args} { return 1 }
            proc dservAddMatch {args} { return 1 }
            proc dpointSetScript {args} { return 1 }
            
            # Stub timer/gpio
            proc timerSetScript {args} { return 1 }
            proc timerTick {args} { return 1 }
            proc gpioLineSetValue {args} { return 1 }
            
            # Stub remote commands
            proc rmtSend {args} { return "" }
            proc rmtOpen {args} { return 1 }
            proc rmtConnected {args} { return 0 }
            
            # Stub juicer
            proc essJuice {args} { return 1 }
            proc reward {args} { return 1 }
            
            # Stub ain (analog input)
            proc ainSetIndexedParam {args} { return 1 }
            proc ainSetIndexedParams {args} { return 1 }
            proc ainGetIndexedParam {args} { return 0 }
            
            # Stub process commands
            proc processSetParam {args} { return 1 }
            proc processGetParam {args} { return 0 }
            
            # Stub now command
            proc now {} { return [clock microseconds] }
            
            # Stub eventlog
            proc essFileIO {args} { return 1 }
            
            # Stub send
            proc send {subprocess args} { return 1 }
            
            # Allow package require to succeed silently
            proc package {cmd args} {
                if {$cmd eq "require"} {
                    return "1.0"
                }
                return ""
            }
        }
        
        # Add type-specific stubs
        switch -exact $type {
            system {
                interp eval $interp {
                    proc ess::create_system {name} {
                        return "::test_system"
                    }
                }
            }
            
            protocol {
                interp eval $interp {
                    # Protocol-specific stubs
                    variable ::test_system_ns "::test_ns"
                    namespace eval ::test_ns {
                        proc configure_stim {args} { return 1 }
                        proc set_param {args} { return 1 }
                    }
                }
            }
            
            variants {
                interp eval $interp {
                    namespace eval Variants {
                        variable variants_dict
                    }
                }
            }
            
            loaders {
                interp eval $interp {
                    # Loader stubs
                    proc dg_create {args} { return "test_dg" }
                    proc dg_delete {args} { return 1 }
                    proc dg_exists {args} { return 0 }
                    proc dl_create {args} { return "test_dl" }
                }
            }
            
            stim {
                interp eval $interp {
                    # Stimulus stubs
                    proc stimBegin {args} { return 1 }
                    proc stimEnd {args} { return 1 }
                    proc stimMakeObject {args} { return "obj1" }
                }
            }
        }
        
        return $interp
    }
    
    # Test if script can be sourced safely
    proc safe_source_test {type content} {
        set errors {}
        set warnings {}
        
        # First do syntax check
        set syntax_result [check_syntax $content]
        if {![dict get $syntax_result valid]} {
            return $syntax_result
        }
        set warnings [dict get $syntax_result warnings]
        
        # Create isolated test interpreter
        set test_interp [create_test_interp $type]
        
        # Try to evaluate the script
        set eval_error ""
        set eval_result [catch {
            interp eval $test_interp $content
        } eval_error eval_info]
        
        # Get error details if failed
        if {$eval_result != 0} {
            # Try to extract line number from error info
            set error_line 0
            if {[dict exists $eval_info -errorline]} {
                set error_line [dict get $eval_info -errorline]
            }
            
            lappend errors [dict create \
                line $error_line \
                message $eval_error \
                type "source_error"]
        }
        
        # Cleanup
        interp delete $test_interp
        
        set valid [expr {[llength $errors] == 0}]
        
        return [dict create \
            valid $valid \
            errors $errors \
            warnings $warnings \
            level "source_test"]
    }
    
    #=========================================================================
    # Type-Specific Validation
    #=========================================================================
    
    # Validate variants.tcl specifically
    proc validate_variants {content} {
        set result [safe_source_test "variants" $content]
        if {![dict get $result valid]} {
            return $result
        }
        
        set warnings [dict get $result warnings]
        set errors [dict get $result errors]
        
        # Additional variant-specific checks
        
        # Check for required variant structure
        if {![regexp {^\s*namespace\s+eval\s+Variants\s*\{} $content]} {
            lappend warnings [dict create \
                line 1 \
                message "Expected 'namespace eval Variants' at start"]
        }
        
        # Check each variant definition
        set variant_pattern {(\w+)\s*\{\s*description\s+}
        set found_variants [regexp -all -inline $variant_pattern $content]
        
        if {[llength $found_variants] == 0} {
            lappend warnings [dict create \
                line 0 \
                message "No variant definitions found (expected: name \{ description ... \})"]
        }
        
        # Check for required fields in variants
        foreach {match variant_name} $found_variants {
            # Find this variant's block and check contents
            if {![regexp "${variant_name}\\s*\\{.*?loader_proc\\s+\\w+" $content]} {
                lappend warnings [dict create \
                    line 0 \
                    message "Variant '$variant_name' missing loader_proc"]
            }
        }
        
        return [dict create \
            valid [expr {[llength $errors] == 0}] \
            errors $errors \
            warnings $warnings \
            variants_found [expr {[llength $found_variants] / 2}]]
    }
    
    # Validate protocol.tcl specifically
    proc validate_protocol {content} {
        set result [safe_source_test "protocol" $content]
        if {![dict get $result valid]} {
            return $result
        }
        
        set warnings [dict get $result warnings]
        set errors [dict get $result errors]
        
        # Check for state definitions
        set state_count [regexp -all {ess::state\s+\w+} $content]
        if {$state_count == 0} {
            lappend warnings [dict create \
                line 0 \
                message "No state definitions found"]
        }
        
        # Check for start state
        if {![regexp {set_start\s+\w+} $content]} {
            lappend warnings [dict create \
                line 0 \
                message "No start state defined (set_start)"]
        }
        
        return [dict create \
            valid [expr {[llength $errors] == 0}] \
            errors $errors \
            warnings $warnings \
            state_count $state_count]
    }
    
    #=========================================================================
    # High-Level Validation
    #=========================================================================
    
    # Main validation entry point
    proc validate {type content {level "source_test"}} {
        switch -exact $level {
            syntax {
                return [check_syntax $content]
            }
            
            source_test {
                return [safe_source_test $type $content]
            }
            
            full {
                # Type-specific validation
                switch -exact $type {
                    variants { return [validate_variants $content] }
                    protocol { return [validate_protocol $content] }
                    default  { return [safe_source_test $type $content] }
                }
            }
            
            default {
                error "Unknown validation level: $level (expected: syntax, source_test, full)"
            }
        }
    }
    
    #=========================================================================
    # Validated Save
    #=========================================================================
    
    # Save script only if validation passes
    proc validated_save {type content args} {
        # Parse options
        set apply 0
        set level "source_test"
        set backup 1
        
        foreach {opt val} $args {
            switch -exact -- $opt {
                -apply   { set apply $val }
                -level   { set level $val }
                -backup  { set backup $val }
            }
        }
        
        # Validate
        set validation [validate $type $content $level]
        
        if {![dict get $validation valid]} {
            set error_msg "Validation failed:\n"
            foreach err [dict get $validation errors] {
                append error_msg "  Line [dict get $err line]: [dict get $err message]\n"
            }
            error $error_msg
        }
        
        # Create backup if requested
        if {$backup} {
            catch { ::ess::backup_script $type }
        }
        
        # Save using ess::save_script (which does its own validation too)
        set save_result [::ess::save_script $type $content "fast"]
        
        # Apply (reload) if requested
        if {$apply} {
            if {[catch {::ess::reload_system} reload_err]} {
                # Save succeeded but reload failed
                # This is a warning, not a failure
                return [dict create \
                    success 1 \
                    saved 1 \
                    reloaded 0 \
                    reload_error $reload_err \
                    warnings [dict get $validation warnings]]
            }
        }
        
        return [dict create \
            success 1 \
            saved 1 \
            reloaded $apply \
            warnings [dict get $validation warnings]]
    }
    
    #=========================================================================
    # Recovery Helpers
    #=========================================================================
    
    # Get list of available backups for a script type
    proc list_backups {type} {
        set backup_dir [::ess::get_backup_directory $type]
        
        if {![file exists $backup_dir]} {
            return {}
        }
        
        set backups {}
        foreach f [glob -nocomplain -directory $backup_dir "*_${type}_*.tcl"] {
            set mtime [file mtime $f]
            lappend backups [dict create \
                path $f \
                filename [file tail $f] \
                modified [clock format $mtime] \
                timestamp $mtime]
        }
        
        # Sort by timestamp descending (newest first)
        set backups [lsort -command {apply {{a b} {
            expr {[dict get $b timestamp] - [dict get $a timestamp]}
        }}} $backups]
        
        return $backups
    }
    
    # Restore from a backup file
    proc restore_backup {backup_path} {
        if {![file exists $backup_path]} {
            error "Backup file not found: $backup_path"
        }
        
        # Read backup content
        set fh [open $backup_path r]
        set content [read $fh]
        close $fh
        
        # Determine type from filename
        set fname [file tail $backup_path]
        set type ""
        foreach t {system protocol loaders variants stim} {
            if {[string match "*_${t}_*" $fname]} {
                set type $t
                break
            }
        }
        
        if {$type eq ""} {
            error "Cannot determine script type from backup filename: $fname"
        }
        
        # Validate before restoring
        set validation [validate $type $content]
        if {![dict get $validation valid]} {
            error "Backup file failed validation: [dict get $validation errors]"
        }
        
        # Restore
        ::ess::save_script $type $content "fast"
        
        return [dict create \
            success 1 \
            type $type \
            restored_from $backup_path]
    }
}
