set here [file dirname [file normalize [info script]]]
set soc [file normalize "$here/../.."]
proc setting {name fallback} {
    if {[info exists ::env($name)]} { return $::env($name) }
    return $fallback
}
set mhz [setting SOC_GEMV_HW_MHZ 150]
set burst [setting SOC_GEMV_HW_BURST 64]
set outstanding [setting SOC_GEMV_HW_OUTSTANDING 8]
set stage [setting SOC_GEMV_HW_STAGE csim]
set part [setting SOC_GEMV_HW_PART xczu3eg-sbva484-1-e]
if {$mhz ni {100 150 200} || $burst ni {1 4 16 64 256} ||
    ![string is integer -strict $outstanding] || $outstanding < 1 || $outstanding > 32 ||
    $stage ni {csim synth cosim export} || $part ni {xczu3eg-sbva484-1-e xczu3eg-sbva484-1-i}} {
    error "Use MHz=100/150/200, burst=1/4/16/64/256, outstanding=1..32, stage=csim/synth/cosim/export, part=-e/-i"
}
set work [file normalize "$here/../hls_work/gemv/f${mhz}_b${burst}_o${outstanding}"]
file mkdir [file dirname $work]
cd [file dirname $work]
open_project -reset [file tail $work]
set_top soc_gemv_hw
set includes "-I$soc/sw/repack -I$soc/lab3_ref/part3"
set flags "-std=c++11 -ffp-contract=off $includes"
add_files "$here/gemv.cpp" -cflags $flags
add_files "$soc/lab3_ref/part3/dot_q8_0_core.cpp" -cflags $flags
add_files -tb "$here/tb.cpp" -cflags $flags
add_files -tb "$soc/sw/repack/soc_repack.c" -cflags "-std=c11 -ffp-contract=off $includes"
open_solution -reset solution -flow_target vivado
set_part $part
create_clock -period [expr {1000.0 / $mhz}] -name default
config_compile -unsafe_math_optimizations false
config_compile -no_signed_zeros false

# Depths cover 64 rows of the largest input width in the co-simulation testbench.
set_directive_interface -mode m_axi -offset slave -bundle weights -depth 19456 \
    -max_read_burst_length $burst -num_read_outstanding $outstanding \
    -max_write_burst_length 2 -num_write_outstanding 1 \
    -max_widen_bitwidth 128 soc_gemv_hw w_quants
set_directive_interface -mode m_axi -offset slave -bundle scales -depth 9728 \
    -max_read_burst_length $burst -num_read_outstanding $outstanding \
    -max_write_burst_length 2 -num_write_outstanding 1 \
    -max_widen_bitwidth 128 soc_gemv_hw w_scales
set_directive_interface -mode m_axi -offset slave -bundle activation -depth 5168 \
    -max_read_burst_length 16 -num_read_outstanding 2 \
    -max_write_burst_length 2 -num_write_outstanding 1 \
    -max_widen_bitwidth 32 soc_gemv_hw x_native
set_directive_interface -mode m_axi -offset slave -bundle output -depth 64 \
    -max_read_burst_length 2 -num_read_outstanding 1 \
    -max_write_burst_length 16 -num_write_outstanding 2 \
    -max_widen_bitwidth 32 soc_gemv_hw y

csim_design
if {$stage ne "csim"} { csynth_design }
if {$stage in {cosim export}} { cosim_design -rtl verilog -trace_level port }
if {$stage eq "export"} {
    export_design -format ip_catalog -rtl verilog -vendor soc.org -library hls -version 1.0
}
puts "P3.1 GEMV HLS stage $stage completed: $work"
exit
