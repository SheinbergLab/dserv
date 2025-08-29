package require yajltcl

# service (systemctl) oriented help functions

proc check_service_status {} {
    if {[catch {exec |& systemctl is-active dserv} status]} {
        return "unknown"
    }
    return [string trim $status]
}

proc get_service_info {} {
    try {
        set active [check_service_status]
        set obj [yajl create #auto]
        $obj map_open
        $obj string "service_name" string "dserv"
        $obj string "status" string $active
        
        if {[catch {exec |& systemctl show dserv --property=MainPID --value } pid]} {
            $obj string "pid" string "unknown"
        } else {
            $obj string "pid" string [string trim $pid]
        }
        
        $obj map_close
        set result [$obj get]
        $obj delete
        return $result
    } on error {msg} {
        return "{\"error\": \"$msg\"}"
    }
}

package require yajltcl

namespace eval update {
    variable api_url "https://api.github.com/repos/SheinbergLab/dserv"
    variable repo_url "https://github.com/SheinbergLab/dserv"
    
    proc detect_platform_and_arch {} {
        global tcl_platform
        
        set os $tcl_platform(os)
        set machine $tcl_platform(machine)
        
        # Map to your artifact naming convention
        switch -glob $machine {
            "x86_64" { set arch "amd64" }
            "aarch64" - "arm64" { set arch "arm64" }
            "armv7*" { set arch "armhf" }
            default { set arch $machine }
        }
        
        switch $os {
            "Linux" { 
                if {[file exists "/etc/debian_version"] || 
                    [file exists "/usr/bin/dpkg"]} {
                    set format "deb"
                } else {
                    set format "binary"
                }
                set platform "linux"
            }
            "Darwin" { 
                set platform "macos"
                set format "pkg"
            }
            default { 
                set platform [string tolower $os]
                set format "binary"
            }
        }
        
        return [list $platform $arch $format]
    }
    
    proc get_latest_version {} {
        variable api_url
        set api_endpoint "${api_url}/releases/latest"
        
        if {[catch {exec curl -s $api_endpoint} json_data]} {
            error "Failed to fetch latest release: $json_data"
        }
        
        set release_data [::yajl::json2dict $json_data]
        return [dict get $release_data tag_name]
    }
    
    proc get_latest_release_info {} {
        variable api_url
        set api_endpoint "${api_url}/releases/latest"
        
        if {[catch {exec curl -s $api_endpoint} json_data]} {
            error "Failed to fetch latest release: $json_data"
        }
        
        return [::yajl::json2dict $json_data]
    }
    
    proc construct_download_url {version platform arch format} {
        variable repo_url
        set base_url "${repo_url}/releases/download/${version}"
        
        switch $format {
            "deb" {
                return "${base_url}/dserv_${version}_${arch}.deb"
            }
            "pkg" {
                # macOS uses different naming after code signing
                return "${base_url}/dserv-${version}-Darwin-signed.pkg"
            }
            "binary" {
                if {$platform eq "macos"} {
                    return "${base_url}/dserv-${platform}-${arch}"
                } else {
                    return "${base_url}/dserv-${platform}-${arch}"
                }
            }
            default {
                error "Unsupported format: $format"
            }
        }
    }
    
    proc get_current_version {} {
        # Method 1: Check datapoint (set during dserv startup)
        if {[dservExists system/version]} {
            set version [dservGet system/version]
            if {$version ne "" && $version ne "unknown"} {
                return $version
            }
        }
        
        # Method 2: Use dserv built-in version command
        if {[catch {dservVersion} version]} {
            # Command might not be available
        } else {
            if {$version ne ""} {
                return $version
            }
        }
        
        # Method 3: Try the executable --version flag
        try {
            set exe [info nameofexecutable]
            if {[catch {exec |& $exe --version} version_output]} {
                # Version command failed
            } else {
                set version_output [string trim $version_output]
                if {[regexp {\d+\.\d+\.\d+} $version_output version]} {
                    return $version
                }
            }
        } on error {msg} {
            # Continue to other methods
        }
        
        # Method 4: Check package manager version
        set pkg_version [get_version_from_package]
        if {$pkg_version ne ""} {
            return $pkg_version
        }
        
        return "unknown"
    }
    
    proc get_version_from_package {} {
	lassign [detect_platform_and_arch] platform arch format
	
	switch $format {
	    "deb" {
		# Query dpkg for installed version, suppress stderr
		if {[catch {exec dpkg-query -W -f=${Version} dserv} version]} {
		    return ""
		}
		return [string trim $version]
	    }
	    "pkg" {
		# On macOS, try to get version from package receipt
		if {[catch {exec pkgutil --pkg-info com.sheinberglab.dserv} pkg_info]} {
		    return ""
		}
		# Parse version from pkg_info output
		if {[regexp {version: ([^\s]+)} $pkg_info -> version]} {
		    return $version
		}
		return ""
	    }
	    default {
		return ""
	    }
	}
    }
    
    proc dict_to_json {dict_data} {
        set obj [yajl create #auto]
        $obj map_open
        
        dict for {key value} $dict_data {
            $obj string $key
            if {[string is boolean $value]} {
                $obj bool $value
            } elseif {[string is integer $value]} {
                $obj number $value
            } else {
                $obj string $value
            }
        }
        
        $obj map_close
        set result [$obj get]
        $obj delete
        return $result
    }
    
    proc check_for_updates {} {
        try {
            set current_version [get_current_version]
            set latest_version [get_latest_version]
            set update_available [expr {$current_version ne $latest_version}]
            
            lassign [detect_platform_and_arch] platform arch format
            
            # Store results in datapoints
            dservSet system/current_version $current_version
            dservSet system/latest_version $latest_version
            dservSet system/update_available $update_available
            dservSet system/platform $platform
            dservSet system/arch $arch
            dservSet system/package_format $format
            
            set result [dict create \
                current_version $current_version \
                latest_version $latest_version \
                update_available $update_available \
                platform $platform \
                arch $arch \
                format $format]
            
            return [dict_to_json $result]
            
        } on error {msg} {
            dservSet system/update_error $msg
            return [dict_to_json [dict create error $msg]]
        }
    }
    
    proc download_latest_release {} {
        try {
            lassign [detect_platform_and_arch] platform arch format
            puts "Detected platform: $platform, arch: $arch, format: $format"
            
            set version [get_latest_version]
            puts "Latest version: $version"
            
            set download_url [construct_download_url $version $platform $arch $format]
            puts "Download URL: $download_url"
            
            switch $format {
                "deb" { set filename "dserv_${version}_${arch}.deb" }
                "pkg" { set filename "dserv-${version}-Darwin-signed.pkg" }
                "binary" { set filename "dserv-${platform}-${arch}" }
            }
            
            # Remove existing file if it exists
            if {[file exists $filename]} {
                file delete $filename
            }
            
            # Use curl for download (more reliable than wget)
            puts "Downloading $filename..."
            if {[catch {exec |& curl -L -f --progress-bar -o $filename $download_url} curl_output]} {
                # Check if file exists despite curl error
                if {[file exists $filename] && [file size $filename] > 1000} {
                    puts "curl reported error but file appears downloaded: $curl_output"
                } else {
                    error "Download failed: $curl_output"
                }
            }
            
            # Verify download
            if {![file exists $filename]} {
                error "Downloaded file not found: $filename"
            }
            
            set size [file size $filename]
            if {$size < 1000} {
                file delete $filename
                error "Downloaded file is too small ($size bytes) - likely an error page"
            }
            
            puts "Successfully downloaded: $filename ($size bytes)"
            dservSet system/update_download_file $filename
            dservSet system/update_download_size $size
            dservSet system/update_status "downloaded"
            return $filename
            
        } on error {msg} {
            puts "Error downloading release: $msg"
            dservSet system/update_error $msg
            dservSet system/update_status "download_failed"
            return ""
        }
    }
    
    proc download_status_to_json {status filename size error_msg} {
        set obj [yajl create #auto]
        $obj map_open
        
        $obj string "status" string $status
        
        if {$filename ne ""} {
            $obj string "filename" string $filename
        }
        
        if {$size > 0} {
            $obj string "size" number $size
        }
        
        if {$error_msg ne ""} {
            $obj string "error" string $error_msg
        }
        
        $obj map_close
        set result [$obj get]
        $obj delete
        return $result
    }
    
    proc install_update {filename} {
	lassign [detect_platform_and_arch] platform arch format
	
	dservSet system/update_status "installing"
	
	try {
	    switch $format {
		"deb" {
		    puts "Installing .deb package: $filename"
		    
		    # First, inspect the package
		    if {[catch {exec |& dpkg -I $filename} package_info]} {
			puts "Warning: Could not inspect package: $package_info"
		    } else {
			puts "Package info:\n$package_info"
		    }
		    
		    # Install the package
		    if {[catch {exec |& sudo dpkg -i $filename} result]} {
			puts "dpkg install failed, trying to fix dependencies..."
			catch {exec |& sudo apt-get install -f -y}
			if {[catch {exec |& sudo dpkg -i $filename} result2]} {
			    error "Failed to install .deb package: $result2"
			}
		    }
		    puts "Successfully installed .deb package"
		    
		    # For systemd service, restart via systemctl
		    puts "Restarting dserv service..."
		    if {[catch {exec |& sudo systemctl restart dserv} restart_result]} {
			puts "Warning: Could not restart service via systemctl: $restart_result"
			puts "You may need to restart the service manually"
			dservSet system/update_status "installed_manual_restart_needed"
		    } else {
			puts "Service restarted successfully"
			dservSet system/update_status "installed_and_restarted"
			# The service restart will terminate this process
		    }
		    return 1
		}
		"pkg" {
		    puts "Installing .pkg package: $filename"
		    if {[catch {exec |& sudo installer -pkg $filename -target /} result]} {
			error "Failed to install .pkg package: $result"
		    }
		    puts "Successfully installed .pkg package"
		    dservSet system/update_status "installed"
		    return 1
		}
		"binary" {
		    puts "Replacing binary: $filename"
		    set current_exe [info nameofexecutable]
		    set backup_exe "${current_exe}.backup"
		    
		    # Create backup
		    file copy $current_exe $backup_exe
		    
		    # Replace with new binary
		    file copy -force $filename $current_exe
		    file attributes $current_exe -permissions 0755
		    
		    puts "Successfully replaced binary"
		    dservSet system/update_status "installed"
		    return 1
		}
		default {
		    error "Unsupported package format: $format"
		}
	    }
	} on error {msg} {
	    dservSet system/update_status "install_failed"
	    dservSet system/update_error $msg
	    error $msg
	}
	return 0
    }
    
    proc perform_full_update {} {
        dservSet system/update_status "starting"
        dservSet system/update_error ""
        
        set filename [download_latest_release]
        if {$filename eq ""} {
            set error_msg [dservGet system/update_error]
            return [download_status_to_json "download_failed" "" 0 $error_msg]
        }
        
        if {[install_update $filename]} {
            # Clean up downloaded file
            if {[file exists $filename]} {
                file delete $filename
            }
            
            lassign [detect_platform_and_arch] platform arch format
            
            if {$format eq "deb"} {
                # For .deb packages, systemctl restart is handled in install_update
                puts "Update complete. Service restart handled by systemctl."
                # Note: The systemctl restart will terminate this process
                set size [dservGet system/update_download_size]
                return [download_status_to_json "success" $filename $size "Service will restart"]
                
            } elseif {$format eq "binary"} {
                puts "Update complete. Restarting in 2 seconds..."
                dservSet system/update_status "restarting"
                after 2000 updateRestart
                set size [dservGet system/update_download_size]
                return [download_status_to_json "success" $filename $size "Binary restart in progress"]
                
            } else {
                # pkg or other formats
                puts "Update complete. Package manager handled the installation."
                dservSet system/update_status "complete"
                set size [dservGet system/update_download_size]
                return [download_status_to_json "success" $filename $size ""]
            }
        }
        
        set error_msg [dservGet system/update_error]
        return [download_status_to_json "install_failed" $filename 0 $error_msg]
    }
    
    proc rollback {} {
        lassign [detect_platform_and_arch] platform arch format
        
        if {$format eq "binary"} {
            set current_exe [info nameofexecutable]
            set backup_exe "${current_exe}.backup"
            
            if {[file exists $backup_exe]} {
                try {
                    file copy -force $backup_exe $current_exe
                    file attributes $current_exe -permissions 0755
                    puts "Rollback successful"
                    dservSet system/update_status "rolled_back"
                    return 1
                } on error {msg} {
                    puts "Rollback failed: $msg"
                    dservSet system/update_error $msg
                    return 0
                }
            } else {
                puts "No backup file found for rollback"
                return 0
            }
        } else {
            puts "Rollback not supported for package installations"
            return 0
        }
    }
    
    # Convenience functions for web interface
    proc get_update_status {} {
        if {[dservExists system/update_status]} {
            return [dservGet system/update_status]
        } else {
            return "idle"
        }
    }
    
    proc get_update_info_json {} {
        try {
            set current_version [get_current_version]
            set latest_version [get_latest_version]
            set update_available [expr {$current_version ne $latest_version}]
            set status [get_update_status]
            
            lassign [detect_platform_and_arch] platform arch format
            
            set obj [yajl create #auto]
            $obj map_open
            
            $obj string "current_version" string $current_version
            $obj string "latest_version" string $latest_version  
            $obj string "update_available" bool $update_available
            $obj string "platform" string $platform
            $obj string "arch" string $arch
            $obj string "format" string $format
            $obj string "status" string $status
            
            if {[dservExists system/update_error]} {
                set error [dservGet system/update_error]
                if {$error ne ""} {
                    $obj string "error" string $error
                }
            }
            
            $obj map_close
            set result [$obj get]
            $obj delete
            return $result
            
        } on error {msg} {
            set obj [yajl create #auto]
            $obj map_open
            $obj string "error" string $msg
            $obj map_close
            set result [$obj get]
            $obj delete
            return $result
        }
    }
}

puts "Update module loaded - auto-update functionality available"
