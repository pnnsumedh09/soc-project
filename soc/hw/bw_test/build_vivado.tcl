set here [file dirname [file normalize [info script]]]
proc setting {name fallback} {
    if {[info exists ::env($name)]} { return $::env($name) }
    return $fallback
}
set mhz [setting SOC_BW_MHZ 150]
set burst [setting SOC_BW_BURST 64]
set outstanding [setting SOC_BW_OUTSTANDING 8]
set board_part [setting SOC_BW_BOARD_PART ""]
set ps_config [setting SOC_BW_PS_CONFIG ""]
set jobs [setting SOC_BW_JOBS 8]
if {$mhz ni {100 150 200} || $burst ni {1 4 16 64 256} ||
    ![string is integer -strict $outstanding] || $outstanding < 1 || $outstanding > 32 ||
    ![string is integer -strict $jobs] || $jobs < 1} {
    error "Invalid MHz, burst, outstanding request count, or job count"
}
if {($board_part eq "") == ($ps_config eq "")} {
    error "Set exactly one of SOC_BW_BOARD_PART or SOC_BW_PS_CONFIG (see README.md)"
}
if {$ps_config ne "" && ![file isfile $ps_config]} { error "Missing PS configuration: $ps_config" }
set tag f${mhz}_b${burst}_o${outstanding}
set ip_repo [file normalize "$here/../hls_work/bw_test/$tag/solution/impl/ip"]
if {![file isfile "$ip_repo/component.xml"]} { error "Export HLS IP first: $ip_repo" }
set work [file normalize "$here/../vivado/bw_test_$tag"]
create_project -force bw_test $work -part xczu3eg-sbva484-1-e
if {$board_part ne ""} { set_property board_part $board_part [current_project] }
set_property ip_repo_paths [list $ip_repo] [current_project]
update_ip_catalog
add_files "$here/read-window.v"
create_bd_design soc_bw
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.3 zynq_ultra_ps_e_0]
if {$board_part ne ""} {
    apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e \
        -config {apply_board_preset "1"} $ps
} else {
    # This file must configure the existing $ps cell, not create a second PS.
    source [file normalize $ps_config]
}
set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0 {1} \
    CONFIG.PSU__USE__M_AXI_GP1 {0} \
    CONFIG.PSU__USE__M_AXI_GP2 {0} \
    CONFIG.PSU__MAXIGP0__DATA_WIDTH {32} \
    CONFIG.PSU__USE__S_AXI_GP0 {0} \
    CONFIG.PSU__USE__S_AXI_GP1 {0} \
    CONFIG.PSU__USE__S_AXI_GP2 {1} \
    CONFIG.PSU__USE__S_AXI_GP3 {0} \
    CONFIG.PSU__USE__S_AXI_GP4 {0} \
    CONFIG.PSU__USE__S_AXI_GP5 {0} \
    CONFIG.PSU__USE__S_AXI_GP6 {0} \
    CONFIG.PSU__SAXIGP2__DATA_WIDTH {128} \
    CONFIG.PSU__USE__FABRIC__RST {1} \
    CONFIG.PSU__FPGA_PL0_ENABLE {1} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ $mhz] $ps

create_bd_cell -type ip -vlnv soc.org:hls:soc_bw_read:1.0 bw_read_0
create_bd_cell -type module -reference soc_bw_read_window read_window_0
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 clear_0
set_property -dict [list CONFIG.C_GPIO_WIDTH {1} CONFIG.C_ALL_OUTPUTS {1}] [get_bd_cells clear_0]
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 cycles_0
set_property -dict [list CONFIG.C_IS_DUAL {1} CONFIG.C_GPIO_WIDTH {32} \
    CONFIG.C_GPIO2_WIDTH {32} CONFIG.C_ALL_INPUTS {1} CONFIG.C_ALL_INPUTS_2 {1}] [get_bd_cells cycles_0]
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 control_xbar
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {3}] [get_bd_cells control_xbar]
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 memory_xbar
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] [get_bd_cells memory_xbar]
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 reset_0
set_property CONFIG.C_EXT_RESET_HIGH {0} [get_bd_cells reset_0]
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 locked_0
set_property CONFIG.CONST_VAL {1} [get_bd_cells locked_0]

connect_bd_intf_net [get_bd_intf_pins $ps/M_AXI_HPM0_FPD] [get_bd_intf_pins control_xbar/S00_AXI]
foreach {master slave} {M00_AXI bw_read_0/s_axi_control M01_AXI clear_0/S_AXI M02_AXI cycles_0/S_AXI} {
    connect_bd_intf_net [get_bd_intf_pins control_xbar/$master] [get_bd_intf_pins $slave]
}
connect_bd_intf_net [get_bd_intf_pins bw_read_0/m_axi_mem] [get_bd_intf_pins memory_xbar/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins memory_xbar/M00_AXI] [get_bd_intf_pins $ps/S_AXI_HP0_FPD]
foreach pin {maxihpm0_fpd_aclk saxihp0_fpd_aclk} {
    connect_bd_net [get_bd_pins $ps/pl_clk0] [get_bd_pins $ps/$pin]
}
foreach pin {bw_read_0/ap_clk read_window_0/clk clear_0/s_axi_aclk cycles_0/s_axi_aclk control_xbar/aclk memory_xbar/aclk reset_0/slowest_sync_clk} {
    connect_bd_net [get_bd_pins $ps/pl_clk0] [get_bd_pins $pin]
}
connect_bd_net [get_bd_pins $ps/pl_resetn0] [get_bd_pins reset_0/ext_reset_in]
connect_bd_net [get_bd_pins locked_0/dout] [get_bd_pins reset_0/dcm_locked]
foreach pin {bw_read_0/ap_rst_n read_window_0/resetn clear_0/s_axi_aresetn cycles_0/s_axi_aresetn control_xbar/aresetn memory_xbar/aresetn} {
    connect_bd_net [get_bd_pins reset_0/peripheral_aresetn] [get_bd_pins $pin]
}
connect_bd_net [get_bd_pins clear_0/gpio_io_o] [get_bd_pins read_window_0/clear]
connect_bd_net [get_bd_pins read_window_0/cycles_lo] [get_bd_pins cycles_0/gpio_io_i]
connect_bd_net [get_bd_pins read_window_0/cycles_hi] [get_bd_pins cycles_0/gpio2_io_i]
foreach {axi probe} {ARVALID arvalid ARREADY arready RVALID rvalid RREADY rready} {
    connect_bd_net [get_bd_pins bw_read_0/m_axi_mem_$axi] [get_bd_pins read_window_0/$probe]
}
assign_bd_address
validate_bd_design
save_bd_design
set bd [get_files */soc_bw.bd]
generate_target all $bd
set wrapper [make_wrapper -files $bd -top]
add_files -norecurse $wrapper
set_property top soc_bw_wrapper [current_fileset]
update_compile_order -fileset sources_1
launch_runs impl_1 -to_step write_bitstream -jobs $jobs
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} { error "Implementation failed" }
open_run impl_1
report_timing_summary -file "$work/timing_summary.rpt"
report_utilization -file "$work/utilization.rpt"
foreach delay_type {max min} {
    set paths [get_timing_paths -delay_type $delay_type -max_paths 1]
    if {[llength $paths] == 0 || [get_property SLACK [lindex $paths 0]] < 0} {
        error "Timing not met ($delay_type); inspect $work/timing_summary.rpt"
    }
}
set out [file normalize "$here/../out"]
file mkdir $out
set stem "$out/bw_test_$tag"
set hwh "$work/bw_test.gen/sources_1/bd/soc_bw/hw_handoff/soc_bw.hwh"
if {![file isfile $hwh]} { error "Find the generated soc_bw.hwh under $work before board use" }
file copy -force "$work/bw_test.runs/impl_1/soc_bw_wrapper.bit" "$stem.bit"
file copy -force $hwh "$stem.hwh"
write_hw_platform -fixed -include_bit -force -file "$stem.xsa"
puts "P1.1 bitstream and HWH: $stem"
close_project
