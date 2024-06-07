set currentTheme [tablelist::getCurrentTheme]
if {$tablelist::themeDefaults(-stripebackground) eq "" &&
    $currentTheme ne "black"} {
    option add *Tablelist.background            white
    option add *Tablelist.stripeBackground      #f0f0f0
}
if {[tk windowingsystem] eq "x11"} {
    option add *Font              TkDefaultFont
    option add *selectBackground  $tablelist::themeDefaults(-selectbackground)
    option add *selectForeground  $tablelist::themeDefaults(-selectforeground)
}
option add *selectBorderWidth     $tablelist::themeDefaults(-selectborderwidth)
