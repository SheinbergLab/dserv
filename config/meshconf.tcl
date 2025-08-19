puts "initializing mesh networking"

# Check if mesh is enabled (set by dserv.cpp)
if {![info exists ::mesh_enabled]} {
    puts "Mesh networking not enabled (use --mesh command line option)"
    return
}

# Subscribe to mesh-relevant datapoints
dservAddMatch ess/status
dservAddMatch ess/experiment  
dservAddMatch ess/participant

# Datapoint handler for automatic status updates
proc mesh_datapoint_handler { dpoint data } {
    switch $dpoint {
        "ess/status" {
            meshUpdateStatus $data
        }
        "ess/experiment" {
            meshUpdateExperiment $data
        }
        "ess/participant" {
            meshUpdateParticipant $data
        }
    }
}

# Connect the handler to all mesh datapoints
dpointSetScript ess/status mesh_datapoint_handler
dpointSetScript ess/experiment mesh_datapoint_handler
dpointSetScript ess/participant mesh_datapoint_handler

#################################################################
# CORE MESH FUNCTIONS - Built on C++ commands
#################################################################

# Update mesh status (uses the C++ command)
proc mesh_update_status { status } {
    set current_experiment [expr {[dservExists ess/experiment] ? [dservGet ess/experiment] : ""}]
    set current_participant [expr {[dservExists ess/participant] ? [dservGet ess/participant] : ""}]
    meshSetStatus $status $current_experiment $current_participant
}

proc mesh_update_experiment { experiment } {
    set current_status [expr {[dservExists ess/status] ? [dservGet ess/status] : "idle"}]
    set current_participant [expr {[dservExists ess/participant] ? [dservGet ess/participant] : ""}]
    meshSetStatus $current_status $experiment $current_participant
}

proc mesh_update_participant { participant } {
    set current_status [expr {[dservExists ess/status] ? [dservGet ess/status] : "idle"}]
    set current_experiment [expr {[dservExists ess/experiment] ? [dservGet ess/experiment] : ""}]
    meshSetStatus $current_status $current_experiment $participant
}

#################################################################
# HIGH-LEVEL MESH HELPER FUNCTIONS - Pure Tcl
#################################################################

# Get cluster status as a nice Tcl dict
proc mesh_get_cluster_status {} {
    return [meshGetPeers]
}

# Get just the appliance IDs
proc mesh_get_appliance_ids {} {
    set peers [meshGetPeers]
    set ids {}
    foreach peer $peers {
        lappend ids [dict get $peer id]
    }
    return $ids
}

# Check if a specific appliance is online
proc mesh_appliance_online { appliance_id } {
    set ids [mesh_get_appliance_ids]
    return [expr {$appliance_id in $ids}]
}

# Get status of specific appliance
proc mesh_get_appliance_status { appliance_id } {
    set peers [meshGetPeers]
    foreach peer $peers {
        if {[dict get $peer id] eq $appliance_id} {
            return [dict get $peer status]
        }
    }
    return ""
}

# Broadcast status to mesh (via datapoints)
proc mesh_broadcast_status { status } {
    dservSet ess/status $status
    # This automatically triggers mesh_datapoint_handler
}

# Start experiment across cluster
proc mesh_start_experiment { experiment_name participant_id } {
    dservSet ess/experiment $experiment_name
    dservSet ess/participant $participant_id
    dservSet ess/status "running"
    puts "Started experiment: $experiment_name for participant: $participant_id"
}

# Stop experiment 
proc mesh_stop_experiment {} {
    dservSet ess/status "idle"
    puts "Experiment stopped"
}

# Wait for all appliances to reach a certain status
proc mesh_wait_for_status { target_status {appliance_list {}} {timeout_ms 30000} } {
    set start_time [clock milliseconds]
    
    if {$appliance_list eq {}} {
        set appliance_list [mesh_get_appliance_ids]
    }
    
    while {[clock milliseconds] - $start_time < $timeout_ms} {
        set all_ready 1
        foreach appliance_id $appliance_list {
            set status [mesh_get_appliance_status $appliance_id]
            if {$status ne $target_status} {
                set all_ready 0
                break
            }
        }
        
        if {$all_ready} {
            return 1
        }
        
        after 1000  ;# Wait 1 second
    }
    
    return 0  ;# Timeout
}

# Synchronize experiment start across multiple appliances
proc mesh_sync_experiment_start { experiment_name participant_id {appliances {}} } {
    puts "Synchronizing experiment start across cluster..."
    
    # Set experiment info
    mesh_start_experiment $experiment_name $participant_id
    
    # Wait for all appliances to be ready
    if {[mesh_wait_for_status "running" $appliances]} {
        puts "All appliances ready for experiment"
        return 1
    } else {
        puts "Timeout waiting for appliances to be ready"
        return 0
    }
}

# Get summary of cluster state
proc mesh_cluster_summary {} {
    set peers [meshGetPeers]
    set summary [dict create total [llength $peers] running 0 idle 0 error 0]
    
    foreach peer $peers {
        set status [dict get $peer status]
        dict incr summary $status
    }
    
    return $summary
}

#################################################################
# HTML GENERATION - Tcl-based templating with live data
#################################################################

# Generate the mesh dashboard HTML with current datapoint values
# Generate the mesh dashboard HTML using template engine
proc mesh_generate_html {} {
    # Set template variables
    templateEngine setVar "hostname" [expr {[dservExists system/hostname] ? [dservGet system/hostname] : "Unknown"}]
    templateEngine setVar "current_status" [expr {[dservExists ess/status] ? [dservGet ess/status] : "idle"}]
    templateEngine setVar "current_experiment" [expr {[dservExists ess/experiment] ? [dservGet ess/experiment] : ""}]
    templateEngine setVar "current_participant" [expr {[dservExists ess/participant] ? [dservGet ess/participant] : ""}]
    
    # Set status color based on current status
    switch [templateEngine setVar "current_status"] {
        "running" { templateEngine setVar "status_color" "#4CAF50" }
        "idle" { templateEngine setVar "status_color" "#ffc107" }
        "error" { templateEngine setVar "status_color" "#f44336" }
        default { templateEngine setVar "status_color" "#6c757d" }
    }
    
    # Set conditionals
    templateEngine setConditional "show_experiment" [expr {[dservExists ess/experiment] && [dservGet ess/experiment] ne ""}]
    templateEngine setConditional "show_participant" [expr {[dservExists ess/participant] && [dservGet ess/participant] ne ""}]
    
    # Build template string
    set template {<!DOCTYPE html>
<html>
<head>
    <title>Mesh Dashboard - {{hostname}}</title>
    <style>
        body { 
            font-family: 'Inter', Arial, sans-serif; 
            margin: 20px; 
            background: linear-gradient(135deg, #f5f7fa 0%, #c3cfe2 100%);
            min-height: 100vh;
        }
        .header {
            background: white;
            padding: 20px;
            border-radius: 12px;
            box-shadow: 0 4px 16px rgba(0,0,0,0.1);
            margin-bottom: 20px;
        }
        .status-indicator {
            display: inline-block;
            width: 12px;
            height: 12px;
            border-radius: 50%;
            background: {{status_color}};
            margin-right: 8px;
        }
        .appliance-panel { 
            border: 1px solid #e1e5e9; 
            margin: 10px 0; 
            padding: 20px; 
            border-radius: 12px;
            background: white;
            box-shadow: 0 2px 8px rgba(0,0,0,0.05);
            transition: all 0.3s ease;
        }
        .appliance-panel:hover {
            transform: translateY(-2px);
            box-shadow: 0 8px 24px rgba(0,0,0,0.15);
        }
        .appliance-panel.local { 
            background: linear-gradient(135deg, #e8f5e8 0%, #f0fff0 100%);
            border-color: #4CAF50;
        }
        .appliance-panel.running { border-left: 5px solid #4CAF50; }
        .appliance-panel.idle { border-left: 5px solid #ffc107; }
        .appliance-panel.error { border-left: 5px solid #f44336; }
        .status { font-weight: 600; margin: 8px 0; color: #2c3e50; }
        .experiment-info { color: #6c757d; font-size: 0.95em; margin: 4px 0; }
        .refresh-btn { 
            margin: 10px 0; 
            padding: 12px 24px; 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white; 
            border: none; 
            border-radius: 6px; 
            cursor: pointer;
            font-weight: 500;
            transition: all 0.3s ease;
        }
        .refresh-btn:hover {
            transform: translateY(-1px);
            box-shadow: 0 4px 12px rgba(102, 126, 234, 0.4);
        }
        .current-info {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 15px;
            margin-bottom: 20px;
        }
        .info-card {
            background: white;
            padding: 15px;
            border-radius: 8px;
            box-shadow: 0 2px 8px rgba(0,0,0,0.05);
        }
        .info-label { font-size: 0.85em; color: #6c757d; margin-bottom: 5px; }
        .info-value { font-weight: 600; color: #2c3e50; }
    </style>
</head>
<body>
    <div class="header">
        <h1><span class="status-indicator"></span>Mesh Dashboard - {{hostname}}</h1>
        
        <div class="current-info">
            <div class="info-card">
                <div class="info-label">Current Status</div>
                <div class="info-value">{{current_status}}</div>
            </div>
            {{#if show_experiment}}
            <div class="info-card">
                <div class="info-label">Active Experiment</div>
                <div class="info-value">{{current_experiment}}</div>
            </div>
            {{/if}}
            {{#if show_participant}}
            <div class="info-card">
                <div class="info-label">Participant</div>
                <div class="info-value">{{current_participant}}</div>
            </div>
            {{/if}}
            <div class="info-card">
                <div class="info-label">Last Updated</div>
                <div class="info-value" id="lastUpdate">{{tcl: clock format [clock seconds] -format "%H:%M:%S"}}</div>
            </div>
        </div>
    </div>
    
    <button class="refresh-btn" onclick="loadPeers()">Refresh Cluster Status</button>
    <div id="appliances"></div>
    
    <script>
        function loadPeers() {
            document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
            
            fetch('/api/mesh/peers')
                .then(response => response.json())
                .then(data => {
                    const appliancesDiv = document.getElementById('appliances');
                    appliancesDiv.innerHTML = '';
                    
                    data.appliances.forEach(appliance => {
                        const panel = document.createElement('div');
                        panel.className = 'appliance-panel ' + appliance.status + (appliance.isLocal ? ' local' : '');
                        
                        let html = '<h3>' + appliance.name + (appliance.isLocal ? ' (Local)' : '') + '</h3>';
                        html += '<div class="status">Status: ' + appliance.status + '</div>';
                        
                        if (appliance.currentExperiment) {
                            html += '<div class="experiment-info">Experiment: ' + appliance.currentExperiment + '</div>';
                        }
                        if (appliance.participantId) {
                            html += '<div class="experiment-info">Participant: ' + appliance.participantId + '</div>';
                        }
                        if (!appliance.isLocal && appliance.ipAddress) {
                            html += '<div class="experiment-info">IP: ' + appliance.ipAddress + ':' + appliance.webPort + '</div>';
                        }
                        
                        panel.innerHTML = html;
                        appliancesDiv.appendChild(panel);
                    });
                })
                .catch(error => {
                    console.error('Error loading peers:', error);
                    document.getElementById('appliances').innerHTML = 
                        '<div style="color: #dc3545; padding: 20px;">Error loading cluster status</div>';
                });
        }
        
        // Load peers on page load
        loadPeers();
        
        // Auto-refresh every 10 seconds
        setInterval(loadPeers, 10000);
    </script>
</body>
</html>}
    
    return [templateRenderString $template]
}

# Generate simpler HTML for testing
proc mesh_generate_simple_html {} {
    set hostname [expr {[dservExists system/hostname] ? [dservGet system/hostname] : "Unknown"}]
    set status [expr {[dservExists ess/status] ? [dservGet ess/status] : "idle"}]
    
    return "<html><body><h1>$hostname</h1><p>Status: $status</p><p>Time: [clock format [clock seconds]]</p></body></html>"
}

puts "Mesh configuration ready - using built-in mesh manager"
puts "Available commands:"
puts "  meshGetPeers / mesh_get_cluster_status"
puts "  meshUpdateStatus <status> / meshUpdateExperiment <exp> / meshUpdateParticipant <participant>"
puts "  mesh_broadcast_status <status>"
puts "  mesh_start_experiment <experiment> <participant>"
puts "  mesh_stop_experiment"
puts "  mesh_sync_experiment_start <experiment> <participant> \[appliances\]"
puts "  mesh_wait_for_status <status> \[appliances\] \[timeout_ms\]"
puts "  mesh_appliance_online <appliance_id>"
puts "  mesh_generate_html / mesh_generate_simple_html"
