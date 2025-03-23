#
# Procs for interacting with a datahub
#


set dlshlib [file join /usr/local dlsh dlsh.zip]
if [file exists $dlshlib] {
    set base [file join [zipfs root] dlsh]
   zipfs mount $dlshlib $base
   set auto_path [linsert $auto_path [set auto_path 0] $base/lib]
}

package require qpcs

proc print { args } { puts $args }
set server localhost

if { [qpcs::dsRegister $server 2] != 1 } {
    error "Unable to register with $server"
}
qpcs::dsAddCallback print
qpcs::dsAddMatch $server myvar

qpcs::dsSet $server myvar hello

vwait forever
