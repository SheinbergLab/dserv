package require ess

proc show_vals {} {
    print "sampler_average: [dservGet ain/samplers/0/vals]"
}

proc start_sampler {} {
    if { [dservGet gpio/input/22] == 0 } {
	ess::em_sampler_start 0
    }
}

ess::em_init
ess::em_fixwin_set 0 4 4 8
ess::em_sampler_enable 1000

gpioLineRequestInput 22
dservAddExactMatch gpio/input/22
dpointSetScript gpio/input/22 { start_sampler }

dservAddExactMatch ain/samplers/0/vals
dpointSetScript ain/samplers/0/vals { show_vals }

