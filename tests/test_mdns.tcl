# test_mdns.tcl -- exercises modules/mdns (the `_dserv._tcp` DNS-SD advert).
#
# Run under tests/test_mdns_host (a bare interp exporting the tclserver_*
# stand-ins the module binds to):
#
#   test_mdns_host tests/test_mdns.tcl build/modules/dserv_mdns.dylib
#
# Two layers. The argument and state checks need no responder and run
# everywhere. The live checks (register, watch the responder grant the
# name, update TXT, withdraw) run when a responder answers -- mDNSResponder
# on any Mac, avahi-daemon on a rig -- and are reported as skipped where
# none does (a CI container), since dserv itself treats that the same way:
# log once, carry on without an advertisement.

if { [llength $argv] < 1 } {
    error "usage: test_mdns.tcl <path to dserv_mdns module>"
}
load [lindex $argv 0]

set checks 0
# cond is an expr evaluated in the caller's scope, so a check can be
# written as {[dict get $i registered]} and read like the claim it makes.
proc check { what cond } {
    global checks
    incr checks
    if { ![uplevel 1 [list expr $cond]] } { error "FAILED: $what" }
    puts "  ok: $what"
}

# ---- commands + initial state -------------------------------------------
foreach c {mdnsRegister mdnsUpdate mdnsUnregister mdnsInfo} {
    check "$c exists" [llength [info commands $c]]
}
set i [mdnsInfo]
check "not registered at load" {![dict get $i registered]}
check "state unregistered at load" {[dict get $i state] eq "unregistered"}
check "default type" {[dict get $i type] eq "_dserv._tcp"}
check "default port" {[dict get $i port] == 2560}

# ---- argument validation (never reaches the responder) ------------------
check "odd arg count rejected" \
    {[catch {mdnsRegister -name} e] && [string match {*-name N*} $e]}
check "unknown option rejected" \
    {[catch {mdnsRegister -bogus 1} e] && [string match {*bad option*} $e]}
check "port 0 rejected" \
    {[catch {mdnsRegister -port 0} e] && [string match {*out of range*} $e]}
check "port 70000 rejected" \
    {[catch {mdnsRegister -port 70000} e] && [string match {*out of range*} $e]}
check "non-numeric port rejected" {[catch {mdnsRegister -port abc}]}
check "64-byte name rejected" \
    {[catch {mdnsRegister -name [string repeat x 64]} e] &&
     [string match {*63 bytes*} $e]}
check "TXT value > 255 rejected" \
    {[catch {mdnsRegister -txt [list k [string repeat v 256]]} e] &&
     [string match {*255 bytes*} $e]}
check "malformed TXT dict rejected" {[catch {mdnsRegister -txt {a b c}}]}
check "update before register rejected" \
    {[catch {mdnsUpdate {a b}} e] && [string match {*not registered*} $e]}
check "unregister when not registered is a no-op" \
    {![catch {mdnsUnregister}]}
check "still unregistered after rejects" {![dict get [mdnsInfo] registered]}

# ---- live: needs a responder ---------------------------------------------
# A private type + a unique instance name, so a test never collides with a
# real dserv on the same link and two test runs never rename each other.
set name "dserv-test-[pid]"
set txt  [dict create web 2565 newline 2570 ssl 0 wg test]
if { [catch {
    mdnsRegister -name $name -type _dservtest._tcp -port 2560 -txt $txt
} err] } {
    check "register failure names the dns_sd error" \
        {[string match {*dns_sd error*} $err]}
    check "failure leaves state as error" \
        {[string match {error *} [dict get [mdnsInfo] state]]}
    check "failure leaves nothing registered" {![dict get [mdnsInfo] registered]}
    puts "  (no mDNS responder here -- live checks skipped: $err)"
    puts "all checks passed ($checks checks)"
    exit 0
}

check "register returns the requested name" {$err eq $name}
check "registered flag set" {[dict get [mdnsInfo] registered]}
check "TXT echoed back" {[dict get [mdnsInfo] txt] eq $txt}

# The responder answers asynchronously; the callback thread turns that
# into state "registered <granted>".
set deadline [expr {[clock milliseconds] + 5000}]
while { [clock milliseconds] < $deadline } {
    if { [string match {registered *} [dict get [mdnsInfo] state]] } break
    after 50
}
set i [mdnsInfo]
check "responder granted the registration" \
    {[string match {registered *} [dict get $i state]]}
check "granted name is ours (no conflict on a fresh name)" \
    {[dict get $i granted] eq $name}

# TXT replaced in place; still registered
mdnsUpdate [dict replace $txt wg other]
set i [mdnsInfo]
check "update keeps registration" {[dict get $i registered]}
check "update replaces TXT" {[dict get $i txt wg] eq "other"}

# Re-register replaces (one record, never two)
mdnsRegister -name $name -type _dservtest._tcp -port 2561 -txt $txt
check "re-register takes the new port" {[dict get [mdnsInfo] port] == 2561}
check "re-register is registered" {[dict get [mdnsInfo] registered]}

mdnsUnregister
set i [mdnsInfo]
check "unregister clears flag" {![dict get $i registered]}
check "unregister clears granted" {[dict get $i granted] eq ""}
check "unregister reports state" {[dict get $i state] eq "unregistered"}
check "update after unregister rejected" {[catch {mdnsUpdate {a b}}]}

puts "all checks passed ($checks checks)"
