#
# show stimdg in realtime
#
package require Tk
package require dlsh
package require qpcs
package require tablelist

if { 0 } {
    if {[tk windowingsystem] eq "x11"} {
	#
	# Create the font TkDefaultFont if not yet present
	#
	catch {font create TkDefaultFont -family Helvetica -size 9}
	
	option add *DgView*Font                        TkDefaultFont
	option add *DgView*selectBackground            #5294e2
	option add *DgView*selectForeground            white
    }
    option add *DgView.tf.borderWidth                  1
    option add *DgView.tf.relief                       sunken
    option add *DgView.tf.tbl.borderWidth              0
    option add *DgView.tf.tbl.highlightThickness       0
    option add *DgView.tf.tbl.background               white
    option add *DgView.tf.tbl.stripeBackground         #f0f0f0
    option add *DgView.tf.tbl.setGrid                  yes
    option add *DgView.tf.tbl*Entry.background         white
    option add *DgView.bf.Button.width                 10
}

proc dg_view { dg { parent {} } } {
    if { $parent == {} } { set top {} } { set top $parent }
    #
    # Create a vertically scrolled tablelist widget with dynamic-width
    # columns and interactive sort capability within the toplevel
    #
    set tf $top.tf
    if { [winfo exists $tf] } { destroy $tf }
    frame $tf
    set tbl $tf.tbl
    set vsb $tf.vsb
    set hsb $tf.hsb

    set colinfo {}
    set maxrows 0

    #
    # Create column list and determine lengths
    #
    set colnames [dg_tclListnames $dg]
    foreach c $colnames {
	set colinfo "$colinfo 0 $c left"
	set collen [dl_length $dg:$c]
	if { $collen > $maxrows } { set maxrows $collen }
    }

    set ncols [llength $colnames]
    set nrows $maxrows

    tablelist::tablelist $tbl \
        -columns $colinfo \
        -xscrollcommand [list $hsb set] -yscrollcommand [list $vsb set] \
	-setgrid no -width 0
    if {[$tbl cget -selectborderwidth] == 0} {
        $tbl configure -spacing 1
    }

    scrollbar $vsb -orient vertical -command [list $tbl yview]    
    scrollbar $hsb -orient horizontal -command [list $tbl xview]

    for { set i 0 } { $i < $ncols } { incr i } {
	$tbl columnconfigure $i -maxwidth 22
    }
    

    #
    # Manage the widgets
    #
    grid $tbl -row 0 -rowspan 2 -column 0 -sticky news
    if {[tk windowingsystem] eq "win32"} {
        grid $vsb -row 0 -rowspan 2 -column 1 -sticky ns
    } else {
        grid [$tbl cornerpath] -row 0 -column 1 -sticky ew
        grid $vsb              -row 1 -column 1 -sticky ns
    }
    grid $hsb -row 2 -column 0 -sticky ew
    grid rowconfigure    $tf 1 -weight 1
    grid columnconfigure $tf 0 -weight 1
    pack $tf -side top -expand yes -fill both


    #
    # Fill the table
    #
    for { set i 0 } { $i < $nrows } { incr i } {
	set row {}
	for { set j 0 } { $j < $ncols } { incr j } {
	    set c [lindex $colnames $j]
	    set entry [dl_tcllist $dg:$c:$i]
	    if { $entry != {} } { lappend row $entry } { lappend row {} }
	}
	$tbl insert end $row
    }

    return $tbl
}

# to stimdg is sent as b64 encoded string, this proc unpacks into stim
proc receive_stimdg { ds args } {
    set g [dg_fromString64 [lindex $args 4]]
    show_stimdg $g
}

proc show_stimdg { dg } {
    dg_view $dg
}

qpcs::dsRegister localhost
qpcs::dsAddMatch localhost stimdg
qpcs::dsAddCallback receive_stimdg
qpcs::dsTouch localhost stimdg
