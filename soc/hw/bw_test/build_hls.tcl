set here [file dirname [file normalize [info script]]]
proc setting {name fallback} {
    if {[info exists ::env($name)]} { return $::env($name) }
    return $fallback
}
set mhz [setting SOC_BW_MHZ 150]
set burst [setting SOC_BW_BURST 64]
set outstanding [setting SOC_BW_OUTSTANDING 8]
set stage [setting SOC_BW_STAGE export]
set part [setting SOC_BW_PART xczu3eg-sbva484-1-e]
if {$mhz ni {100 150 200} || $burst ni {1 4 16 64 256} ||
    ![string is integer -strict $outstanding] || $outstanding < 1 || $outstanding > 32 ||
    $stage ni {csim synth cosim export}} {
    error "Use MHz=100/150/200, burst=1/4/16/64/256, outstanding=1..32, stage=csim/synth/cosim/export"
}
if {$part ni {xczu3eg-sbva484-1-e xczu3eg-sbva484-1-i}} {
    error "SOC_BW_PART must identify the Ultra96 ZU3EG device (-e or -i)"
}
set work [file normalize "$here/../hls_work/bw_test/f${mhz}_b${burst}_o${outstanding}"]
file mkdir [file dirname $work]
cd [file dirname $work]
open_project -reset [file tail $work]
set_top soc_bw_read
add_files "$here/bw-read.cpp" -cflags "-std=c++11"
add_files -tb "$here/tb.cpp" -cflags "-std=c++11"
open_solution -reset solution -flow_target vivado
set_part $part
create_clock -period [expr {1000.0 / $mhz}] -name default
# Depth sizes the co-simulation adapter; the RTL limit is checked in the kernel.
set_directive_interface -mode m_axi -offset slave -bundle mem -depth 65536 \
    -max_read_burst_length $burst -num_read_outstanding $outstanding \
    -max_write_burst_length 2 -num_write_outstanding 1 \
    -max_widen_bitwidth 128 soc_bw_read mem
csim_design
if {$stage ne "csim"} { csynth_design }
if {$stage in {cosim export}} { cosim_design -rtl verilog -trace_level port }
if {$stage eq "export"} {
    export_design -format ip_catalog -rtl verilog -vendor soc.org -library hls -version 1.0
}
puts "P1.1 HLS stage $stage completed: $work"
exit
