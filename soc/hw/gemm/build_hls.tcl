set here [file dirname [file normalize [info script]]]

proc setting {name fallback} {
    if {[info exists ::env($name)]} { return $::env($name) }
    return $fallback
}

set stage [setting SOC_GEMM_HW_STAGE csim]
set mhz [setting SOC_GEMM_HW_MHZ 100]
set burst [setting SOC_GEMM_HW_BURST 64]
set outstanding [setting SOC_GEMM_HW_OUTSTANDING 8]
set part [setting SOC_GEMM_HW_PART xczu3eg-sbva484-1-i]

if {$stage ni {csim synth cosim export} ||
    $mhz ni {100 150 200} ||
    $burst ni {1 4 16 64 256} ||
    ![string is integer -strict $outstanding] ||
    $outstanding < 1 || $outstanding > 32 ||
    $part ni {xczu3eg-sbva484-1-e xczu3eg-sbva484-1-i}} {
    error "Use stage=csim/synth/cosim/export, MHz=100/150/200, burst=1/4/16/64/256, outstanding=1..32, part=-e/-i"
}

set tag "f${mhz}_b${burst}_o${outstanding}"
set work [file normalize "$here/../hls_work/gemm/$tag"]
file mkdir [file dirname $work]
cd [file dirname $work]

open_project -reset [file tail $work]
set_top soc_gemm_hw
set flags "-std=c++11 -ffp-contract=off"
add_files "$here/gemm.cpp" -cflags $flags
add_files -tb "$here/tb.cpp" -cflags $flags

open_solution -reset solution -flow_target vivado
set_part $part
create_clock -period [expr {1000.0 / $mhz}] -name default
config_compile -unsafe_math_optimizations=false
config_compile -no_signed_zeros=false

# Co-simulation depths cover the largest testbench shape, not the runtime maxima.
set_directive_interface -mode m_axi -offset slave -bundle weights -depth 4864 \
    -max_read_burst_length $burst -num_read_outstanding $outstanding \
    -max_write_burst_length 2 -num_write_outstanding 1 \
    -max_widen_bitwidth 128 soc_gemm_hw w_quants
set_directive_interface -mode m_axi -offset slave -bundle scales -depth 2432 \
    -max_read_burst_length $burst -num_read_outstanding $outstanding \
    -max_write_burst_length 2 -num_write_outstanding 1 \
    -max_widen_bitwidth 128 soc_gemm_hw w_scales
set_directive_interface -mode m_axi -offset slave -bundle activation -depth 41344 \
    -max_read_burst_length 16 -num_read_outstanding 4 \
    -max_write_burst_length 2 -num_write_outstanding 1 \
    -max_widen_bitwidth 32 soc_gemm_hw x_native
set_directive_interface -mode m_axi -offset slave -bundle output -depth 128 \
    -max_read_burst_length 2 -num_read_outstanding 1 \
    -max_write_burst_length 16 -num_write_outstanding 2 \
    -max_widen_bitwidth 32 soc_gemm_hw y

csim_design
if {$stage ne "csim"} {
    csynth_design
}
if {$stage in {cosim export}} {
    cosim_design -rtl verilog -trace_level port
}
if {$stage eq "export"} {
    export_design -format ip_catalog -rtl verilog \
        -vendor soc.org -library hls -version 1.0
}

puts "Q4_0/Q8_0 GEMM HLS stage $stage completed: $work"
exit
