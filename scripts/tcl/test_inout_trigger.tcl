proc process_in_out {} { puts "in_out: [triggerData]" }

triggerAdd ain/proc/in_out 1 process_in_out

# add in_out processor
set path [file dir [info nameofexecutable]]
processLoad [file join $path processors in_out[info sharedlibextension]] in_out
processAttach in_out ain/vals in_out

puts "loaded inout triggers"
