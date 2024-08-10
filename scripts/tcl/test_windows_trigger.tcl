proc process_windows {} { puts "windows_status: [triggerData]" }

triggerAdd proc/windows/status 1 process_windows

# add in_out processor
set path [file dir [info nameofexecutable]]
processLoad [file join $path processors windows[info sharedlibextension]] windows
processAttach windows ain/vals windows

puts "loaded windows"
