dservAddExactMatch proc/windows/status
#dpointSetScript proc/windows/status {puts [dservGet proc/windows/status] }
dpointSetScript proc/windows/status {puts proc/windows/status}
puts "loaded windows configuration"
