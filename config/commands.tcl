# commands that will be available from main dserv interp

proc dg_get { p name } { 
   set exists [send $p "dg_exists $name"]
   if { $exists } {
      send $p [list dg_toString $name __dg__]
      set g [dg_rename [dg_fromString [getVar $p __dg__]] [dg_tempname]]
      send $p { unset __dg__ }
      return $g
   }
}

proc dg_view { g } {
  # see if the name has a '/'
  set splitname [split $g /]

  # if so, process/dg manes get dg from process
  if { [llength $splitname] == 2 } {
      lassign $splitname p dg
      dg_rename [getDg $p $dg] $p/$dg
      dservSet ess/dev_dg_data [dg_toHybridJSON $p/$dg]
      dg_delete $p/$dg
   } else {
      dservSet ess/dev_dg_data [dg_toHybridJSON $g]
   }
}

proc flushwin { { target graphics/dev } } {
   dservSet $target [dumpwin json]
}