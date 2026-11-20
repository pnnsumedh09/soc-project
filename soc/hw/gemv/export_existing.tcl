# Package an existing sequential synthesis, without reset, C sim, synth or cosim.
set here [file dirname [file normalize [info script]]]
proc setting {key default} {
    if {[info exists ::env($key)]} { return $::env($key) }
    return $default
}
set mhz [setting SOC_GEMV_HW_MHZ 150]
set burst [setting SOC_GEMV_HW_BURST 64]
set outstanding [setting SOC_GEMV_HW_OUTSTANDING 8]
set part [setting SOC_GEMV_HW_PART xczu3eg-sbva484-1-e]
if {$mhz ni {100 150 200} || $burst ni {1 4 16 64 256} ||
    $part ni {xczu3eg-sbva484-1-e xczu3eg-sbva484-1-i} ||
    ![string is integer -strict $outstanding] || $outstanding < 1 || $outstanding > 32 ||
    [setting SOC_GEMV_HW_ROW_INTERLEAVE 1] ne "1"} { error "Select a valid sequential configuration" }
set tag f${mhz}_b${burst}_o${outstanding}
set work [file normalize "$here/../hls_work/gemv/$tag"]
set report "$work/solution/syn/report/soc_gemv_hw_csynth"
foreach suffix {xml rpt} {
    if {![file isfile "$report.$suffix"]} { error "Missing existing synthesis: $report.$suffix" }
}
set f [open "$report.xml" r]; set xml [read $f]; close $f
set f [open "$report.rpt" r]; set rpt [read $f]; close $f
if {![regexp {<Part>([^<]+)</Part>} $xml -> actual] || $actual ne $part ||
    [string first "read_words" $rpt] < 0 || [string first "interleave_words" $rpt] >= 0} {
    error "Existing report does not match the requested sequential target"
}
if {![regexp {<TargetClockPeriod>([^<]+)</TargetClockPeriod>} $xml -> period] ||
    abs($period - 1000.0 / $mhz) > 0.02} { error "Existing synthesis clock differs from requested MHz" }
if {![file isfile "$work/solution/syn/verilog/soc_gemv_hw.v"]} { error "Existing synthesized Verilog is missing" }
cd [file dirname $work]
open_project [file tail $work]
open_solution solution
export_design -format ip_catalog -rtl verilog -vendor soc.org -library hls -version 1.0
source "$here/check_export.tcl"
soc_gemv_check_registers "$work/solution/impl/ip"
puts "Packaged existing synthesis: $work/solution/impl/ip (no simulation or synthesis rerun)"
exit
