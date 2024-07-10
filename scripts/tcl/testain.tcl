set server localhost
proc update_eye_position { x y } {
    dl_local coords [dl_create short $y $x]
    set ds [qpcs::dsSocketOpen $::server]
    qpcs::dsSocketSetData $ds ain/vals $coords
    close $ds
}


proc touch_position { x y } {
    dl_local coords [dl_create short $x $y]
    set ds [qpcs::dsSocketOpen $::server]
    qpcs::dsSocketSetData $ds mtouch/touchvals $coords
    close $ds
}

