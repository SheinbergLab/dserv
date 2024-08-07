package require dlsh
package require qpcs
package require tablelist_tile

if {[tk windowingsystem] eq "x11" &&
    ($tk_version < 8.7 || [package vcompare $::tk_patchLevel "8.7a5"] <= 0)} {
    #
    # Patch the default theme's styles TCheckbutton and TRadiobutton
    #
    package require themepatch
    themepatch::patch default
}

proc decode_params { type len data } {
    
    # ESS_DS_STRING
    if { $type == 1 } { return $data }
    
    # Other types are base64 encoded
    set d [dict create 0 char 1 string 2 float 4 short 5 long]
    set datatype [dict get $d $type]
    dl_local dl [dl_create $datatype]
    dl_fromString64 $data $dl
    
    set d [dict create char 1 float 4 short 2 long 4]
    set eltsize [dict get $d $datatype]
    if { [dl_length $dl] != [expr $len/$eltsize] } {
	error "invalid data received"
    }
    return [dl_tcllist $dl]
}

proc process_events { args } {
    global widgets essinfo
    lassign $args t varname dtype ts dlen dbuf
    switch -glob $varname {
	evt:* {
	    lassign [split $varname :] e etype esubtype
	    if { $etype == 3 } {
		if { $esubtype == 2 } {
		    $::tbl delete 0 end
		    set ::last_obst $ts
		}
	    }
	    if { $etype == 1 } {
		set ::type_names($esubtype) $dbuf
	    }
	    if { $etype == 6 } {
		dict for { k v } $dbuf {
		    set ::subtype_names($esubtype,$v) $k
		}
	    }
	    
	    if { $etype == 18 } {
		if { $esubtype == 0 } { set ::essinfo(ESS) $dbuf }
		if { $esubtype == 1 } { set ::essinfo(SUBJECT) $dbuf }
		if { $esubtype == 2 } { set ::essinfo(PROTOCOL) $dbuf }
		if { $esubtype == 3 } { set ::essinfo(VARIANT) $dbuf }
	    }
	    
	    if { $etype == 19 } { set ::last_obst $ts }
	    set elapsed_ms [expr {($ts-$::last_obst)/1000}] 
	    set item {}
	    lappend item $elapsed_ms
	    lappend item $::type_names($etype)
	    if { [catch { set stype $::subtype_names($etype,$esubtype) }] } {
		set stype $esubtype
	    }
	    lappend item $stype
	    lappend item [decode_params $dtype $dlen $dbuf]
	    $::tbl insert end $item
	}
    }
}

proc connect_to_server { server } {
    qpcs::dsRegister $server
    qpcs::dsAddCallback process_events
    qpcs::dsAddMatch $server eventlog/events
}

set ::last_obst [clock microseconds]
set dir [file dirname [info script]]
source [file join $dir option_tile.tcl]

set if [ttk::frame .info_frame]

label $if.serverlabel -text Host: -anchor e -width 8
ttk::frame $if.ipframe
#::mentry::ipAddrMentry $if.ipframe.serverentry
ttk::combobox $if.ipframe.serverentry -textvariable server
pack $if.ipframe.serverentry -anchor e
grid $if.serverlabel $if.ipframe  -padx 3
set widgets(server) $if.statusvalue
pack $if -side top

set bf [ttk::frame .button_frame]
set f [ttk::frame .f]
set tbl $f.tbl
set vsb $f.vsb
set hsb $f.hsb

tablelist::tablelist $tbl \
    -columns {
	0 "Timestamp"       left
	0 "Type"            left
	0 "Subtype"         left
	0 "Params"          left} -width 35 \
    -yscrollcommand [list $vsb set] -borderwidth 0

if {[$tbl cget -selectborderwidth] == 0} {
    $tbl configure -spacing 1
}
$tbl columnconfigure 0 -sortmode integer
ttk::scrollbar $vsb -orient vertical -command [list $tbl yview]

set btn [ttk::button $bf.btn -text "Close" -command exit]

#
# Manage the widgets
#
grid $tbl -row 0 -rowspan 2 -column 0 -sticky news
if {[tk windowingsystem] eq "win32"} {
    grid $vsb -row 0 -rowspan 2 -column 1 -sticky ns
} else {
    grid [$tbl cornerpath] -row 0 -column 1 -sticky ew
    grid $vsb	       -row 1 -column 1 -sticky ns
}
grid rowconfigure    $f 1 -weight 1
grid columnconfigure $f 0 -weight 1
#pack $b1 $b2 $b3 -side left -expand yes -pady 7p
pack $btn
pack $bf -side bottom -fill x
pack $f -side top -expand yes -fill both

proc initialize { server } {
    global type_names subtype_names essinfo status widgets
    for { set i 0 } { $i < 16 } { incr i } {
	set type_names($i) Reserved$i
    }
    for { set i 16 } { $i < 128 } { incr i } {
	set type_names($i) System$i
    }
    for { set i 128 } { $i < 256 } { incr i } {
	set type_names($i) User$i
    }
    array set subtype_names {}
    array set essinfo {}

}

if { [llength $argv] > 0 } { set server [lindex $argv 0] } { set server 127.0.0.1 }
initialize $server
connect_to_server $server

