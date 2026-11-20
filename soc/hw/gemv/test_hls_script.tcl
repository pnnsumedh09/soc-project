# Control-flow regression only; this does not replace a Vitis run.
set script [file join [file dirname [file normalize [info script]]] build_hls.tcl]
foreach name [array names ::env SOC_GEMV_HW_*] { unset ::env($name) }
set ::env(SOC_GEMV_HW_STAGE) csim
set fp_options {}
set ran_csim 0
set project_names {}

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
    lappend ::project_names [lindex $args end]
    if {[string first / [lindex $args end]] >= 0} { error "Project name contains a slash" }
}
proc add_files {args} {
    set index [expr {[lindex $args 0] eq "-tb" ? 1 : 0}]
    set path [lindex $args $index]
    if {[file pathtype $path] ne "absolute" || ![file isfile $path]} {
        error "Missing or relative source path: $path"
    }
    if {[file extension $path] eq ".c"} {
        error "Registering a C source exposes it to apcc's C++ options"
    }
}
foreach cmd {set_top open_solution set_part create_clock set_directive_interface} {
    proc $cmd {args} {}
}
proc csim_design {args} {
    if {[llength $args] != 2 || [lindex $args 0] ne "-ldflags" ||
        ![file isfile [lindex $args 1]]} {
        error "C simulation must link the separately compiled C reference object"
    }
    set ::ran_csim 1
}
foreach cmd {csynth_design cosim_design export_design} {
    proc $cmd {args} { error "Unexpected vendor stage in csim-only test" }
}
rename exit real_exit
proc exit {args} {}
source $script
if {!$ran_csim || [lsort $fp_options] ne {-no_signed_zeros=false -unsafe_math_optimizations=false}} {
    error "Missing C simulation dispatch or strict FP options"
}
set ::env(SOC_GEMV_HW_STAGE) cosim
set ran_cosim 0
proc csynth_design {args} {}
proc cosim_design {args} {
    set index [lsearch -exact $args -ldflags]
    if {$index < 0 || ![file isfile [lindex $args [expr {$index + 1}]]]} {
        error "RTL co-simulation must link the compiled C reference too"
    }
    set ::ran_cosim 1
}
source $script
if {!$ran_cosim} { error "Missing RTL co-simulation dispatch" }
# Baseline and candidate must not reset the same generated project.
set ::env(SOC_GEMV_HW_ROW_INTERLEAVE) 1
source $script
if {$project_names ne {f150_b64_o8_r8 f150_b64_o8_r8 f150_b64_o8}} {
    error "Baseline/candidate project separation failed: $project_names"
}
set ::env(SOC_GEMV_HW_ROW_INTERLEAVE) 3
if {![catch {source $script}]} { error "Accepted unsupported interleave count" }
puts "HLS script regression: PASS (vendor commands mocked)"
