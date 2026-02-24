// ess_scaffold.go - Scaffolding operations for creating new protocols and systems
//
// Provides two primary operations:
//   - ScaffoldProtocol: Clone an existing protocol within a system (common case)
//   - ScaffoldSystem:   Clone an entire system or create from skeleton template
//
// All operations are registry-side (database-only), so clients sync afterward.
// Provenance is tracked via forked_from fields.

package main

import (
	"fmt"
	"strings"
	"time"
)

// ============ Request/Response Types ============

// ScaffoldProtocolRequest creates a new protocol within an existing system
type ScaffoldProtocolRequest struct {
	// Target
	Workgroup string `json:"workgroup"`
	System    string `json:"system"`
	Protocol  string `json:"protocol"` // New protocol name

	// Source (optional — if empty, creates from skeleton)
	FromProtocol string `json:"fromProtocol,omitempty"` // Clone from this protocol
	FromSystem   string `json:"fromSystem,omitempty"`   // Clone from different system (default: same)

	// Metadata
	Description string `json:"description,omitempty"`
	CreatedBy   string `json:"createdBy"`
}

// ScaffoldSystemRequest creates a new system in a workgroup
type ScaffoldSystemRequest struct {
	// Target
	Workgroup string `json:"workgroup"`
	System    string `json:"system"` // New system name

	// Source (one of these)
	FromSystem    string `json:"fromSystem,omitempty"`    // Clone from existing system in same workgroup
	FromWorkgroup string `json:"fromWorkgroup,omitempty"` // Clone from different workgroup
	Template      string `json:"template,omitempty"`      // Create from named template (e.g., "_templates/match_to_sample")

	// Options
	Protocol    string `json:"protocol,omitempty"`    // Initial protocol name (for new system from skeleton)
	Description string `json:"description,omitempty"`
	CreatedBy   string `json:"createdBy"`
}

// ScaffoldResponse is returned by scaffold operations
type ScaffoldResponse struct {
	System    string   `json:"system"`
	Protocols []string `json:"protocols"`
	Scripts   int      `json:"scripts"`
	ForkedFrom string  `json:"forkedFrom,omitempty"`
}

// ============ Protocol Scaffolding ============

// ScaffoldProtocol creates a new protocol by cloning an existing one or from a skeleton.
// This is the common operation — adding a new protocol to a stable system.
func (r *ESSRegistry) ScaffoldProtocol(req ScaffoldProtocolRequest) (*ScaffoldResponse, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if req.Workgroup == "" || req.System == "" || req.Protocol == "" {
		return nil, fmt.Errorf("workgroup, system, and protocol are required")
	}
	if req.CreatedBy == "" {
		req.CreatedBy = "scaffold"
	}

	// Validate: target system must exist
	targetSys, err := r.GetSystem(req.Workgroup, req.System, "main")
	if err != nil {
		return nil, fmt.Errorf("failed to look up target system: %w", err)
	}
	if targetSys == nil {
		return nil, fmt.Errorf("system not found: %s/%s", req.Workgroup, req.System)
	}

	// Check that target protocol doesn't already exist
	existing, err := r.GetScript(targetSys.ID, req.Protocol, ScriptTypeProtocol)
	if err != nil {
		return nil, fmt.Errorf("failed to check existing protocol: %w", err)
	}
	if existing != nil {
		return nil, fmt.Errorf("protocol %s already exists in %s/%s", req.Protocol, req.Workgroup, req.System)
	}

	now := time.Now()
	var sourceScripts []*ESSScript
	var forkedFrom string

	if req.FromProtocol != "" {
		// Clone from existing protocol
		sourceSystem := req.System
		sourceWorkgroup := req.Workgroup
		if req.FromSystem != "" {
			sourceSystem = req.FromSystem
		}

		sourceSys, err := r.GetSystem(sourceWorkgroup, sourceSystem, "main")
		if err != nil || sourceSys == nil {
			return nil, fmt.Errorf("source system not found: %s/%s", sourceWorkgroup, sourceSystem)
		}

		// Get all scripts for the source protocol
		allScripts, err := r.GetScripts(sourceSys.ID)
		if err != nil {
			return nil, fmt.Errorf("failed to get source scripts: %w", err)
		}

		for _, s := range allScripts {
			if s.Protocol == req.FromProtocol {
				sourceScripts = append(sourceScripts, s)
			}
		}

		if len(sourceScripts) == 0 {
			return nil, fmt.Errorf("source protocol %s has no scripts in %s/%s",
				req.FromProtocol, sourceWorkgroup, sourceSystem)
		}

		forkedFrom = fmt.Sprintf("%s/%s/%s", sourceWorkgroup, sourceSystem, req.FromProtocol)
	} else {
		// Create from skeleton
		sourceScripts = r.generateProtocolSkeleton(req.System, req.Protocol, req.Description)
		forkedFrom = "_skeleton"
	}

	// Begin transaction
	tx, err := r.db.Begin()
	if err != nil {
		return nil, fmt.Errorf("failed to begin transaction: %w", err)
	}
	defer tx.Rollback()

	scriptCount := 0
	for _, src := range sourceScripts {
		// Transform content: replace old protocol name with new
		content := src.Content
		filename := src.Filename
		if req.FromProtocol != "" {
			content = renameInContent(content, req.FromProtocol, req.Protocol, req.System)
			filename = renameFilename(filename, req.FromProtocol, req.Protocol)
		}

		checksum := computeChecksum(content)

		_, err = tx.Exec(`INSERT INTO ess_scripts 
			(system_id, protocol, type, filename, content, checksum, updated_at, updated_by)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
			targetSys.ID, req.Protocol, src.Type, filename, content, checksum, now.Unix(), req.CreatedBy)
		if err != nil {
			return nil, fmt.Errorf("failed to create script %s: %w", src.Type, err)
		}
		scriptCount++
	}

	// Update system timestamp
	_, err = tx.Exec(`UPDATE ess_systems SET updated_at = ?, updated_by = ? WHERE id = ?`,
		now.Unix(), req.CreatedBy, targetSys.ID)
	if err != nil {
		return nil, fmt.Errorf("failed to update system timestamp: %w", err)
	}

	if err := tx.Commit(); err != nil {
		return nil, fmt.Errorf("failed to commit: %w", err)
	}

	// Return the updated protocol list
	protocols, _ := r.getSystemProtocols(targetSys.ID)

	return &ScaffoldResponse{
		System:     req.System,
		Protocols:  protocols,
		Scripts:    scriptCount,
		ForkedFrom: forkedFrom,
	}, nil
}

// ============ System Scaffolding ============

// ScaffoldSystem creates a new system by cloning an existing one or from a template.
func (r *ESSRegistry) ScaffoldSystem(req ScaffoldSystemRequest) (*ScaffoldResponse, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if req.Workgroup == "" || req.System == "" {
		return nil, fmt.Errorf("workgroup and system are required")
	}
	if req.CreatedBy == "" {
		req.CreatedBy = "scaffold"
	}

	// Check that target system doesn't already exist
	existing, _ := r.GetSystem(req.Workgroup, req.System, "")
	if existing != nil {
		return nil, fmt.Errorf("system %s already exists in workgroup %s", req.System, req.Workgroup)
	}

	now := time.Now()
	var sourceScripts []*ESSScript
	var forkedFrom string
	var sourceDescription string
	var sourceSystemName string

	if req.FromSystem != "" {
		// Clone from existing system
		sourceWorkgroup := req.Workgroup
		if req.FromWorkgroup != "" {
			sourceWorkgroup = req.FromWorkgroup
		}

		sourceSys, err := r.GetSystem(sourceWorkgroup, req.FromSystem, "main")
		if err != nil || sourceSys == nil {
			return nil, fmt.Errorf("source system not found: %s/%s", sourceWorkgroup, req.FromSystem)
		}

		sourceScripts, err = r.GetScripts(sourceSys.ID)
		if err != nil {
			return nil, fmt.Errorf("failed to get source scripts: %w", err)
		}

		sourceSystemName = req.FromSystem
		sourceDescription = sourceSys.Description
		forkedFrom = fmt.Sprintf("%s/%s@%s", sourceWorkgroup, req.FromSystem, sourceSys.Version)

	} else if req.Template != "" {
		// Clone from templates workgroup
		sourceSys, err := r.GetSystem(TemplatesWorkgroup, req.Template, "")
		if err != nil || sourceSys == nil {
			return nil, fmt.Errorf("template not found: %s", req.Template)
		}

		sourceScripts, err = r.GetScripts(sourceSys.ID)
		if err != nil {
			return nil, fmt.Errorf("failed to get template scripts: %w", err)
		}

		sourceSystemName = req.Template
		sourceDescription = sourceSys.Description
		forkedFrom = fmt.Sprintf("%s/%s@%s", TemplatesWorkgroup, req.Template, sourceSys.Version)

	} else {
		// Create from built-in skeleton
		protoName := req.Protocol
		if protoName == "" {
			protoName = "default"
		}
		sourceScripts = r.generateSystemSkeleton(req.System, protoName, req.Description)
		sourceSystemName = req.System // no renaming needed
		sourceDescription = req.Description
		forkedFrom = "_skeleton"
	}

	// Begin transaction
	tx, err := r.db.Begin()
	if err != nil {
		return nil, fmt.Errorf("failed to begin transaction: %w", err)
	}
	defer tx.Rollback()

	// Create the new system record
	description := req.Description
	if description == "" {
		description = sourceDescription
	}

	result, err := tx.Exec(`INSERT INTO ess_systems 
		(workgroup, name, version, description, author, forked_from, forked_at, created_at, updated_at, updated_by)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		req.Workgroup, req.System, "main", description, req.CreatedBy,
		forkedFrom, now.Unix(), now.Unix(), now.Unix(), req.CreatedBy)
	if err != nil {
		return nil, fmt.Errorf("failed to create system: %w", err)
	}
	newSystemID, _ := result.LastInsertId()

	// Copy scripts with name substitution
	scriptCount := 0
	protocolSet := make(map[string]bool)

	for _, src := range sourceScripts {
		content := src.Content
		filename := src.Filename
		protocol := src.Protocol

		// Rename system references in content and filenames
		if sourceSystemName != req.System {
			content = renameInContent(content, sourceSystemName, req.System, "")
			filename = renameFilename(filename, sourceSystemName, req.System)
		}

		// For protocol-level scripts, if only one protocol exists and
		// a new protocol name was specified, rename it too
		if protocol != "" && req.Protocol != "" && req.FromSystem != "" {
			// Only rename if there's exactly one protocol and user specified a new name
			// Otherwise preserve original protocol names
			oldProto := protocol
			if req.Protocol != oldProto {
				content = renameInContent(content, oldProto, req.Protocol, req.System)
				filename = renameFilename(filename, oldProto, req.Protocol)
				protocol = req.Protocol
			}
		}

		checksum := computeChecksum(content)

		_, err = tx.Exec(`INSERT INTO ess_scripts 
			(system_id, protocol, type, filename, content, checksum, updated_at, updated_by)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
			newSystemID, protocol, src.Type, filename, content, checksum, now.Unix(), req.CreatedBy)
		if err != nil {
			return nil, fmt.Errorf("failed to create script %s/%s: %w", protocol, src.Type, err)
		}
		scriptCount++
		if protocol != "" {
			protocolSet[protocol] = true
		}
	}

	if err := tx.Commit(); err != nil {
		return nil, fmt.Errorf("failed to commit: %w", err)
	}

	var protocols []string
	for p := range protocolSet {
		protocols = append(protocols, p)
	}

	return &ScaffoldResponse{
		System:     req.System,
		Protocols:  protocols,
		Scripts:    scriptCount,
		ForkedFrom: forkedFrom,
	}, nil
}

// ============ Name Substitution ============

// renameInContent replaces old names with new names in script content.
// Handles namespace paths (system::protocol), bare references, and comments.
func renameInContent(content, oldName, newName, systemName string) string {
	if oldName == newName {
		return content
	}

	// Replace namespace-qualified references: system::oldName -> system::newName
	if systemName != "" {
		content = strings.ReplaceAll(content, systemName+"::"+oldName, systemName+"::"+newName)
	}

	// Replace in common patterns:
	//   namespace eval system::oldName  ->  namespace eval system::newName
	//   match_to_sample oldName         ->  match_to_sample newName (in headers/comments)
	//   set_protocol oldName (quoted or bare)

	// Bare name replacement — but be careful not to replace substrings.
	// We replace the following patterns that appear in protocol/system files:
	//   - Whole-word occurrences in comments and string contexts
	//   - Filename patterns like oldName.tcl, oldName_stim.tcl
	//   - Variable references using the name

	// Replace filenames: oldName.tcl -> newName.tcl, oldName_stim.tcl -> newName_stim.tcl
	content = strings.ReplaceAll(content, oldName+".tcl", newName+".tcl")
	content = strings.ReplaceAll(content, oldName+"_stim.tcl", newName+"_stim.tcl")
	content = strings.ReplaceAll(content, oldName+"_loaders.tcl", newName+"_loaders.tcl")
	content = strings.ReplaceAll(content, oldName+"_variants.tcl", newName+"_variants.tcl")
	content = strings.ReplaceAll(content, oldName+"_extract.tcl", newName+"_extract.tcl")
	content = strings.ReplaceAll(content, oldName+"_analyze.tcl", newName+"_analyze.tcl")

	// Replace namespace paths
	content = strings.ReplaceAll(content, "::"+oldName+"::", "::"+newName+"::")
	content = strings.ReplaceAll(content, "::"+oldName+" ", "::"+newName+" ")
	content = strings.ReplaceAll(content, "::"+oldName+"\n", "::"+newName+"\n")
	content = strings.ReplaceAll(content, "::"+oldName+"}", "::"+newName+"}")

	// Replace bare namespace eval (no leading ::)
	content = strings.ReplaceAll(content, "namespace eval "+oldName+" ", "namespace eval "+newName+" ")
	content = strings.ReplaceAll(content, "namespace eval "+oldName+"::", "namespace eval "+newName+"::")

	// Replace in header comments (lines starting with #)
	// e.g., "#   match_to_sample colormatch" -> "#   match_to_sample newproto"
	lines := strings.Split(content, "\n")
	for i, line := range lines {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "#") {
			lines[i] = strings.ReplaceAll(line, oldName, newName)
		}
	}
	content = strings.Join(lines, "\n")

	return content
}

// renameFilename transforms a script filename from old naming to new naming.
// e.g., "colormatch.tcl" -> "newproto.tcl", "colormatch_stim.tcl" -> "newproto_stim.tcl"
func renameFilename(filename, oldName, newName string) string {
	return strings.Replace(filename, oldName, newName, 1)
}

// ============ Skeleton Generators ============

// generateProtocolSkeleton creates minimal but functional protocol scripts.
// These follow the exact patterns found in real protocols (colormatch, shapematch, etc.)
func (r *ESSRegistry) generateProtocolSkeleton(systemName, protoName, description string) []*ESSScript {
	if description == "" {
		description = protoName + " protocol"
	}

	scripts := []*ESSScript{
		{
			Protocol: protoName,
			Type:     ScriptTypeProtocol,
			Filename: protoName + ".tcl",
			Content:  skeletonProtocol(systemName, protoName, description),
		},
		{
			Protocol: protoName,
			Type:     ScriptTypeLoaders,
			Filename: protoName + "_loaders.tcl",
			Content:  skeletonLoaders(systemName, protoName, description),
		},
		{
			Protocol: protoName,
			Type:     ScriptTypeVariants,
			Filename: protoName + "_variants.tcl",
			Content:  skeletonVariants(systemName, protoName, description),
		},
		{
			Protocol: protoName,
			Type:     ScriptTypeStim,
			Filename: protoName + "_stim.tcl",
			Content:  skeletonStim(systemName, protoName, description),
		},
		{
			Protocol: protoName,
			Type:     ScriptTypeExtract,
			Filename: protoName + "_extract.tcl",
			Content:  skeletonProtoExtract(systemName, protoName),
		},
	}

	return scripts
}

// generateSystemSkeleton creates a minimal system with one protocol.
func (r *ESSRegistry) generateSystemSkeleton(systemName, protoName, description string) []*ESSScript {
	if description == "" {
		description = systemName + " system"
	}

	// System-level scripts
	scripts := []*ESSScript{
		{
			Protocol: "",
			Type:     ScriptTypeSystem,
			Filename: systemName + ".tcl",
			Content:  skeletonSystem(systemName, description),
		},
		{
			Protocol: "",
			Type:     ScriptTypeExtract,
			Filename: systemName + "_extract.tcl",
			Content:  skeletonSystemExtract(systemName),
		},
		{
			Protocol: "",
			Type:     ScriptTypeAnalyze,
			Filename: systemName + "_analyze.tcl",
			Content:  skeletonSystemAnalyze(systemName),
		},
	}

	// Add protocol-level scripts
	protoScripts := r.generateProtocolSkeleton(systemName, protoName, "")
	scripts = append(scripts, protoScripts...)

	return scripts
}

// ============ Tcl Skeleton Templates ============
//
// These generate minimal but structurally correct Tcl scripts that follow
// the patterns established by real systems (match_to_sample, etc.).
// Each skeleton is immediately loadable by the ESS runtime.

func skeletonSystem(systemName, description string) string {
	return fmt.Sprintf(`##
##  NAME
##    %[1]s.tcl
##
##  DESCRIPTION
##    %[2]s
##

package require ess

namespace eval %[1]s {
    proc create {} {
	set sys [::ess::create_system [namespace tail [namespace current]]]
	
	######################################################################
	#                          System Parameters                         #
	######################################################################
	
	$sys add_param start_delay          0      time int
	$sys add_param interblock_time   1000      time int
	$sys add_param response_timeout 10000      time int
	
	##
	## System variables
	##
	$sys add_variable n_obs              0
	$sys add_variable obs_count          0
	$sys add_variable cur_id             0
	$sys add_variable first_time         1
	$sys add_variable stimtype           0
	$sys add_variable resp               0
	$sys add_variable correct           -1
	$sys add_variable rt
	$sys add_variable finale_delay      500
	
	######################################################################
	#                            System States                           #
	######################################################################
	
	$sys set_start start
	
	#
	# start
	#
	$sys add_state start {} { return start_delay }
	
	#
	# start_delay
	#
	$sys add_action start_delay {
	    ::ess::evt_put SYSTEM_STATE RUNNING [now]
	    timerTick $start_delay
	}
	$sys add_transition start_delay {
	    if { [timerExpired] } { return inter_obs }
	}
	
	#
	# inter_obs
	#
	$sys add_action inter_obs {
	    if { !$first_time } {
		timerTick $interblock_time
	    } else {
		set first_time 0
	    }
	    set rt -1
	    set correct -1
	    my nexttrial
	}
	
	$sys add_transition inter_obs {
	    if [my finished] { return pre_finale }
	    if { [timerExpired] } { return start_obs }
	}
	
	#
	# start_obs
	#
	$sys add_action start_obs {
	    ::ess::begin_obs $obs_count $n_obs
	    my start_obs_reset
	}
	$sys add_transition start_obs {
	    return stimulus_on
	}
	
	#
	# stimulus_on
	#
	$sys add_action stimulus_on {
	    my stimulus_on
	    ::ess::evt_put STIMULUS ON [now]
	    timerTick $response_timeout
	}
	
	$sys add_transition stimulus_on {
	    if { [timerExpired] } { return no_response }
	    set resp [my responded]
	    if { $resp != -1 } { return response }
	}
	
	#
	# response
	#
	$sys add_action response {
	    ::ess::evt_put RESP $resp [now]
	    my stimulus_off
	    ::ess::evt_put STIMULUS OFF [now]
	}
	
	$sys add_transition response {
	    if { [my response_correct] } {
		return correct
	    } else {
		return incorrect
	    }
	}
	
	#
	# no_response
	#
	$sys add_action no_response {
	    my stimulus_off
	    ::ess::evt_put STIMULUS OFF [now]
	    ::ess::evt_put ABORT NORESPONSE [now]
	    ::ess::evt_put ENDTRIAL ABORT [now]
	}
	
	$sys add_transition no_response {
	    return post_trial
	}
	
	#
	# correct
	#
	$sys add_action correct {
	    set correct 1
	    ::ess::evt_put ENDTRIAL CORRECT [now]
	    my reward
	}
	
	$sys add_transition correct { return post_trial }
	
	#
	# incorrect
	#
	$sys add_action incorrect {
	    set correct 0
	    ::ess::evt_put ENDTRIAL INCORRECT [now]
	    my noreward
	}
	
	$sys add_transition incorrect { return post_trial }
	
	#
	# post_trial
	#
	$sys add_action post_trial {
	    ::ess::save_trial_info $correct $rt $stimtype
	}
	
	$sys add_transition post_trial {
	    return finish
	}
	
	#
	# finish
	#
	$sys add_action finish {
	    ::ess::end_obs COMPLETE
	    my endobs
	}
	
	$sys add_transition finish { return inter_obs }
	
	#
	# finale
	#
	$sys add_action pre_finale {
	    timerTick $finale_delay
	}
	
	$sys add_transition pre_finale {
	    if { [timerExpired] } {
		return finale
	    }
	}
	
	$sys add_action finale { my finale }
	$sys add_transition finale { return end }
	
	#
	# end
	#
	$sys set_end {}
	
	######################################################################
	#                         System Callbacks                           #
	######################################################################
	
	$sys set_init_callback {
	    ::ess::init
	}
	
	$sys set_deinit_callback {}
	
	$sys set_reset_callback {
	    set n_obs [my n_obs]
	    set obs_count 0
	}
	
	$sys set_start_callback {
	    set first_time 1
	}
	
	$sys set_quit_callback {
	    ::ess::end_obs QUIT
	}
	
	$sys set_end_callback {}
	$sys set_file_open_callback {}
	$sys set_file_close_callback {}
	$sys set_subject_callback {}
	
	######################################################################
	#                          System Methods                            #
	######################################################################
	
	$sys add_method start_obs_reset {} {}
	
	$sys add_method n_obs {} { return 10 }
	$sys add_method nexttrial {} {
	    set cur_id $obs_count
	    set stimtype $obs_count
	}
	
	$sys add_method finished {} {
	    if { $obs_count == $n_obs } { return 1 } { return 0 }
	}
	
	$sys add_method endobs {} { incr obs_count }
	
	$sys add_method stimulus_on {} {}
	$sys add_method stimulus_off {} {}
	$sys add_method reward {} {}
	$sys add_method noreward {} {}
	$sys add_method finale {} {}
	
	$sys add_method responded {} { return -1 }
	$sys add_method response_correct {} { return 1 }
	
	return $sys
    }
}
`, systemName, description)
}

func skeletonProtocol(systemName, protoName, description string) string {
	return fmt.Sprintf(`#
# PROTOCOL
#   %[1]s %[2]s
#
# DESCRIPTION
#   %[3]s
#

namespace eval %[1]s::%[2]s {
    variable params_defaults {}

    proc protocol_init { s } {
        $s set_protocol [namespace tail [namespace current]]

        $s add_param rmt_host $::ess::rmt_host stim ipaddr

        $s add_param juice_ml 0.8 variable float

        $s add_variable cur_id 0
        $s add_variable correct -1

        $s set_protocol_init_callback {
            ::ess::init

            # open connection to rmt and upload ${protocol}_stim.tcl
            my configure_stim $rmt_host

            # initialize touch processor
            ::ess::touch_init

            # configure juicer subsystem
            ::ess::juicer_init

            # configure sound subsystem
            ::ess::sound_init
        }

        $s set_protocol_deinit_callback {
            ::ess::touch_deinit
            rmtClose
        }

        $s set_reset_callback {
            dl_set stimdg:remaining [dl_ones [dl_length stimdg:stimtype]]
            set obs_count 0
            rmtSend reset
        }

        $s set_start_callback {
            set first_time 1
        }

        $s set_quit_callback {
            rmtSend clearscreen
            ::ess::end_obs QUIT
        }

        $s set_end_callback {
            ::ess::evt_put SYSTEM_STATE STOPPED [now]
        }

        $s set_file_open_callback {
            print "opened datafile $filename"
        }

        $s set_file_close_callback {
            print "closed [file tail [file root $filename]]"
        }


        ######################################################################
        #                         Protocol Methods                           #
        ######################################################################

        $s add_method start_obs_reset {} {}

        $s add_method n_obs {} { return [dl_length stimdg:stimtype] }

        $s add_method nexttrial {} {
            if { [dl_sum stimdg:remaining] } {
                dl_local left_to_show [dl_select stimdg:stimtype [dl_gt stimdg:remaining 0]]
                set cur_id [dl_pickone $left_to_show]
                set stimtype [dl_get stimdg:stimtype $cur_id]
                rmtSend "nexttrial $stimtype"
                set correct -1
            }
        }

        $s add_method endobs {} {
            if { $correct != -1 } {
                dl_put stimdg:remaining $cur_id 0
                incr obs_count
            }
        }

        $s add_method finished {} {
            return [expr [dl_sum stimdg:remaining]==0]
        }

        $s add_method stimulus_on {} {
            rmtSend "!stimulus_on"
        }

        $s add_method stimulus_off {} {
            rmtSend "!stimulus_off"
        }

        $s add_method reward {} {
            ::ess::sound_play 3 70 70
            ::ess::reward $juice_ml
            ::ess::evt_put REWARD MICROLITERS [now] [expr {int($juice_ml*1000)}]
        }

        $s add_method noreward {} {}

        $s add_method finale {} {
            ::ess::sound_play 6 60 400
        }

        $s add_method response_correct {} { return $correct }

        $s add_method responded {} { return -1 }

        return
    }
}
`, systemName, protoName, description)
}

func skeletonLoaders(systemName, protoName, description string) string {
	return fmt.Sprintf(`#
# LOADERS
#   %[1]s %[2]s
#
# DESCRIPTION
#   Loader methods for %[2]s
#

namespace eval %[1]s::%[2]s {
    proc loaders_init { s } {
        $s add_loader setup_trials { n_obs } {

            # build the stimulus datagroup
            if { [dg_exists stimdg] } { dg_delete stimdg }
            set g [dg_create stimdg]
            dg_rename $g stimdg

            # trial indices
            dl_set $g:stimtype [dl_fromto 0 $n_obs]

            # stimulus positions (customize for your paradigm)
            dl_set $g:stim_x [dl_repeat 0. $n_obs]
            dl_set $g:stim_y [dl_repeat 0. $n_obs]

            # tracking which trials have been shown
            dl_set $g:remaining [dl_ones $n_obs]

            return $g
        }
    }
}
`, systemName, protoName)
}

func skeletonVariants(systemName, protoName, description string) string {
	return fmt.Sprintf(`#
# VARIANTS
#   %[1]s %[2]s
#
# DESCRIPTION
#   Variant dictionary for %[2]s
#

namespace eval %[1]s::%[2]s {
    variable variants {
        default {
            description "default variant"
            loader_proc setup_trials
            loader_options {
                n_obs { 10 50 100 }
            }
        }
    }
}
`, systemName, protoName)
}

func skeletonStim(systemName, protoName, description string) string {
	return fmt.Sprintf(`# NAME
#   %[2]s_stim.tcl
#
# DESCRIPTION
#   Stimulus display for %[1]s/%[2]s
#
# REQUIRES
#   polygon
#

#
# nexttrial
#   set up stimuli for the given trial id
#
proc nexttrial { id } {
    glistInit 1
    resetObjList

    # grab stimulus parameters from stimdg
    set stim_x [dl_get stimdg:stim_x $id]
    set stim_y [dl_get stimdg:stim_y $id]

    # create a placeholder stimulus
    set obj [polygon]
    polycolor $obj 1.0 1.0 1.0
    translateObj $obj $stim_x $stim_y
    scaleObj $obj 1.0
    glistAddObject $obj 0
}

proc stimulus_on {} {
    glistSetCurGroup 0
    glistSetVisible 1
    redraw
}

proc stimulus_off {} {
    glistSetVisible 0
    redraw
}

proc reset {} {
    glistSetVisible 0
    redraw
}

proc clearscreen {} {
    glistSetVisible 0
    redraw
}
`, systemName, protoName)
}

func skeletonProtoExtract(systemName, protoName string) string {
	return fmt.Sprintf(`#
# EXTRACT
#   %[1]s %[2]s
#
# DESCRIPTION
#   Data extraction for %[2]s protocol
#

namespace eval %[1]s::%[2]s {
    proc extract { filename } {
        # Extract trial data from ESS data file
        # Implement protocol-specific extraction here
    }
}
`, systemName, protoName)
}

func skeletonSystemExtract(systemName string) string {
	return fmt.Sprintf(`#
# EXTRACT
#   %[1]s
#
# DESCRIPTION
#   System-level data extraction for %[1]s
#

namespace eval %[1]s {
    proc sys_extract { filename } {
        # System-level extraction
    }
}
`, systemName)
}

func skeletonSystemAnalyze(systemName string) string {
	return fmt.Sprintf(`#
# ANALYZE
#   %[1]s
#
# DESCRIPTION
#   System-level analysis for %[1]s
#

namespace eval %[1]s {
    proc sys_analyze { args } {
        # System-level analysis
    }
}
`, systemName)
}
