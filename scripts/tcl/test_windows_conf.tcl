dservAddExactMatch proc/windows/status
proc process_windows_status { args } { return }
dpointSetScript proc/windows/status process_windows_status
puts "loaded windows configuration"
