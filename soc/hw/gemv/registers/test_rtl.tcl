# Standalone AXI-Lite test of existing generated RTL. No HLS rerun or PS preset.
set here [file dirname [file normalize [info script]]]
proc setting {name fallback} {
    if {[info exists ::env($name)]} { return $::env($name) }
    return $fallback
}
proc run_tool {log args} {
    puts "Running: $args"
    set failed [catch {exec -- {*}$args 2>@1} output]
    set f [open $log w]; puts $f $output; close $f
    puts $output
    if {$failed} { error "Tool failed: [lindex $args 0]; inspect $log" }
    return $output
}
proc main {} {
    global here
    set mhz [setting SOC_GEMV_HW_MHZ 150]
    set burst [setting SOC_GEMV_HW_BURST 64]
    set outstanding [setting SOC_GEMV_HW_OUTSTANDING 8]
    if {$mhz ni {100 150 200} || $burst ni {1 4 16 64 256} ||
        ![string is integer -strict $outstanding] || $outstanding < 1 || $outstanding > 32 ||
        [setting SOC_GEMV_HW_ROW_INTERLEAVE 1] ne "1"} { error "Select a valid sequential HLS configuration" }
    set tag f${mhz}_b${burst}_o${outstanding}
    set rtl [file normalize [setting SOC_GEMV_REGISTER_RTL \
        "$here/../../hls_work/gemv/$tag/solution/syn/verilog/soc_gemv_hw_control_s_axi.v"]]
    if {![file isfile $rtl]} { error "Missing generated register bank: $rtl; restore the existing synthesis RTL" }
    foreach tool {xvlog xelab xsim} {
        if {[auto_execok $tool] eq ""} { error "Missing $tool; load the Xilinx 2022.2 module" }
    }
    set work [file normalize [setting SOC_GEMV_REGISTER_WORK "$here/../../hls_work/gemv_registers/$tag"]]
    file mkdir $work
    # Copy inputs for reproducibility; never change the HLS project/RTL.
    file copy -force $rtl "$work/dut_control.v"
    file copy -force "$here/tb_registers.sv" "$work/tb_registers.sv"
    set f [open "$work/manifest.txt" w]
    puts $f "DUT source: $rtl\nTestbench: $here/tb_registers.sv\nStandalone register bank; synthetic compute handshake; no DMA or BRAM model."
    close $f
    cd $work
    run_tool compile_output.log xvlog -sv -work work dut_control.v tb_registers.sv
    run_tool elaborate_output.log xelab work.tb_gemv_registers -s gemv_regs_snapshot -debug typical
    # Never accept a PASS left in the simulator log by an earlier run.
    set f [open xsim.log w]; close $f
    set output [run_tool simulation_output.log xsim gemv_regs_snapshot -runall -log xsim.log]
    if {[file isfile xsim.log]} {
        set f [open xsim.log r]; append output "\n" [read $f]; close $f
    }
    if {[string first "GEMV REGISTER RTL: PASS;" $output] < 0 ||
        [string first "GEMV REGISTER RTL: FAIL" $output] >= 0} {
        error "Register RTL test did not pass; inspect $work/simulation_output.log"
    }
    puts "Register RTL run completed: $work (no kernel synthesis or co-simulation rerun)"
}
if {[catch {main} message]} {
    puts stderr "Register RTL runner failed: $message"
    exit 1
}
exit 0
