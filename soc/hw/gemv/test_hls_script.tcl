# Control-flow regression only; this does not replace a Vitis run.
set script [file join [file dirname [file normalize [info script]]] build_hls.tcl]
foreach name [array names ::env SOC_GEMV_HW_*] { unset ::env($name) }
set ::env(SOC_GEMV_HW_STAGE) csim
set fp_options {}
set ran_csim 0

proc config_compile {args} {
    if {[llength $args] != 1 || [lindex $args 0] ni {
        -unsafe_math_optimizations=false -no_signed_zeros=false
    }} {
        error "Use the Vitis 2022.2 -option=false Boolean syntax"
    }
    lappend ::fp_options [lindex $args 0]
}
if {![catch {config_compile -unsafe_math_optimizations false}]} {
    error "Regression guard accepted the old Boolean syntax"
}
proc open_project {args} {
    if {[string first / [lindex $args end]] >= 0} { error "Project name contains a slash" }
}
proc add_files {args} {
    set index [expr {[lindex $args 0] eq "-tb" ? 1 : 0}]
    set path [lindex $args $index]
    if {[file pathtype $path] ne "absolute" || ![file isfile $path]} {
        error "Missing or relative source path: $path"
    }
}
foreach cmd {set_top open_solution set_part create_clock set_directive_interface} {
    proc $cmd {args} {}
}
proc csim_design {} { set ::ran_csim 1 }
foreach cmd {csynth_design cosim_design export_design} {
    proc $cmd {args} { error "Unexpected vendor stage in csim-only test" }
}
rename exit real_exit
proc exit {args} {}
source $script
if {!$ran_csim || [lsort $fp_options] ne {-no_signed_zeros=false -unsafe_math_optimizations=false}} {
    error "Missing C simulation dispatch or strict FP options"
}
puts "HLS script regression: PASS (vendor commands mocked)"
