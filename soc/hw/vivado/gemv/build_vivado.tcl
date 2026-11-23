set here [file dirname [file normalize [info script]]]
proc setting {name fallback} {
    if {[info exists ::env($name)]} { return $::env($name) }
    return $fallback
}
set mhz [setting SOC_GEMV_HW_MHZ 150]
set burst [setting SOC_GEMV_HW_BURST 64]
set outstanding [setting SOC_GEMV_HW_OUTSTANDING 8]
set board_part [setting SOC_GEMV_BOARD_PART ""]
set ps_config [setting SOC_GEMV_PS_CONFIG ""]
set jobs [setting SOC_GEMV_JOBS 8]
set stage [setting SOC_GEMV_BUILD_STAGE check]
set base [setting SOC_GEMV_CONTROL_BASE 0xA0000000]
if {[setting SOC_GEMV_HW_ROW_INTERLEAVE 1] ne "1"} {
    error "P4 integration currently selects only the sequential kernel"
}
if {![string is wideinteger -strict $base] || $base < 0xA0000000 || $base > 0xAFFF0000 || ($base % 65536) != 0} {
    error "Control base must be 64KiB-aligned in 0xA0000000..0xAFFF0000"
}
set part [setting SOC_GEMV_HW_PART xczu3eg-sbva484-1-e]
set board_repo [setting SOC_GEMV_BOARD_REPO ""]
if {$mhz ni {100 150 200} || $burst ni {1 4 16 64 256} ||
    ![string is integer -strict $outstanding] || $outstanding < 1 || $outstanding > 32 ||
    ![string is integer -strict $jobs] || $jobs < 1} {
    error "Invalid MHz, burst, outstanding request count, or job count"
}
if {$stage ni {check bd bitstream} || $part ni {xczu3eg-sbva484-1-e xczu3eg-sbva484-1-i}} {
    error "Use SOC_GEMV_BUILD_STAGE=check/bd/bitstream and a supported SOC_GEMV_HW_PART (-e or -i)"
}
set tag q4k_f${mhz}_b${burst}_o${outstanding}
set ip_repo [file normalize "$here/../../hls_work/gemv/$tag/solution/impl/ip"]
if {$board_repo ne ""} {
    if {![file isdirectory $board_repo]} { error "Missing board repository: $board_repo" }
    set_param board.repoPaths [concat [get_param board.repoPaths] [list [file normalize $board_repo]]]
}
puts "P4.1 Vivado stage: $stage; HLS configuration: $tag; target part: $part"
puts "Installed Ultra96 board definitions:"
set boards {}
foreach board [get_board_parts -quiet *] {
    if {[string match -nocase *ultra96* $board]} { lappend boards $board }
}
foreach board $boards { puts "  $board  part=[get_property PART_NAME $board]" }
if {[llength $boards] == 0} {
    puts "  None found. Supply the course PS configuration, or register board files using SOC_GEMV_BOARD_REPO."
}
if {![file isfile "$ip_repo/component.xml"]} {
    error "Exported IP component.xml missing: $ip_repo (do not rerun HLS if the existing export can be restored here)"
}
set report [file normalize "$ip_repo/../../syn/report/soc_gemv_hw_csynth.xml"]
if {![file isfile $report]} { error "Missing HLS target-part report: $report" }
set stream [open $report r]
set report_text [read $stream]
close $stream
if {![regexp {<Part>([^<]+)</Part>} $report_text -> hls_part]} { error "Cannot read target part from $report" }
puts "Exported HLS report target: $hls_part"
if {![regexp {<TargetClockPeriod>([^<]+)</TargetClockPeriod>} $report_text -> period] ||
    abs($period - 1000.0 / $mhz) > 0.02} { error "HLS clock target does not match requested MHz" }
source "$here/../../gemv/check_export.tcl"
soc_gemv_check_registers $ip_repo
set rpt [file rootname $report].rpt
if {![file isfile $rpt]} { error "Missing text synthesis report: $rpt" }
set stream [open $rpt r]
set schedule [read $stream]
close $stream
if {[string first "read_quant_words" $schedule] < 0} {
    error "The selected export must contain the Q4_K GEMV kernel"
}
if {$hls_part ne $part} {
    error "HLS target $hls_part differs from requested $part; resolve the actual board/course target before rebuilding"
}
set project_part $part
if {$board_part ne ""} {
    set selected [get_board_parts -quiet $board_part]
    if {[llength $selected] != 1} { error "Board identifier must resolve to exactly one installed definition: $board_part" }
    set board_device [get_property PART_NAME $selected]
    if {$board_device ne $part} {
        # The Fall 2026 course board tutorial explicitly builds HLS for the
        # commercial -e part and creates the Ultra96-V2 Vivado project with
        # Avnet's industrial -i board definition.  Permit only that documented
        # speed/temperature-grade pairing; all other mismatches remain errors.
        if {$part eq "xczu3eg-sbva484-1-e" &&
            $board_device eq "xczu3eg-sbva484-1-i" &&
            [string match -nocase "avnet.com:ultra96v2:*" $board_part]} {
            puts "Course-approved Ultra96-V2 pairing: HLS target $part; Vivado board device $board_device"
        } else {
            error "Board definition selects $board_device, but HLS selects $part; only the documented Avnet Ultra96-V2 -e/-i pairing is accepted"
        }
    }
    set project_part $board_device
}
if {$stage eq "check"} {
    puts "Exported HLS IP and target report found. No block design or bitstream generated."
    puts "Next: select exactly one SOC_GEMV_BOARD_PART or SOC_GEMV_PS_CONFIG, then use SOC_GEMV_BUILD_STAGE=bd."
    return
}
if {($board_part eq "") == ($ps_config eq "")} {
    error "Set exactly one of SOC_GEMV_BOARD_PART or SOC_GEMV_PS_CONFIG (see README.md)"
}
if {$ps_config ne "" && ![file isfile $ps_config]} { error "Missing PS configuration: $ps_config" }
set work [file normalize "$here/../gemv_$tag"]
create_project -force gemv $work -part $project_part
if {$board_part ne ""} { set_property board_part $board_part [current_project] }
set_property ip_repo_paths [list $ip_repo] [current_project]
update_ip_catalog
create_bd_design soc_gemv
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.4 zynq_ultra_ps_e_0]
if {$board_part ne ""} {
    apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e \
        -config {apply_board_preset "1"} $ps
} else {
    # Course file configures this existing $ps, including DDR/MIO/PS clocks.
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
    CONFIG.PSU__USE__IRQ0 {1} \
    CONFIG.PSU__USE__IRQ1 {0} \
    CONFIG.PSU__USE__FABRIC__RST {1} \
    CONFIG.PSU__FPGA_PL0_ENABLE {1} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ $mhz] $ps


create_bd_cell -type ip -vlnv soc.org:hls:soc_gemv_hw:1.0 gemv_0
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 control_xbar
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] [get_bd_cells control_xbar]
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 memory_xbar
set_property -dict [list CONFIG.NUM_SI {4} CONFIG.NUM_MI {1}] [get_bd_cells memory_xbar]
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 reset_0
# Do not choose a tie-off from the initial IP defaults: validation can infer
# different reset polarities. Fix the intended configuration and verify it below.
set_property -dict [list CONFIG.C_EXT_RESET_HIGH {1} CONFIG.C_AUX_RESET_HIGH {0}] [get_bd_cells reset_0]
create_bd_cell -type ip -vlnv xilinx.com:ip:util_vector_logic:2.0 reset_inverter
set_property -dict [list CONFIG.C_OPERATION {not} CONFIG.C_SIZE {1}] [get_bd_cells reset_inverter]
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 locked_0
set_property CONFIG.CONST_VAL {1} [get_bd_cells locked_0]
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 zero_0
set_property CONFIG.CONST_VAL {0} [get_bd_cells zero_0]

connect_bd_intf_net [get_bd_intf_pins $ps/M_AXI_HPM0_FPD] [get_bd_intf_pins control_xbar/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins control_xbar/M00_AXI] [get_bd_intf_pins gemv_0/s_axi_control]
foreach {port slot} {weights S00_AXI metadata S01_AXI activation S02_AXI output_r S03_AXI} {
    set pin [get_bd_intf_pins -quiet gemv_0/m_axi_$port]
    if {[llength $pin] != 1} { error "Missing expected HLS interface m_axi_$port; inspect exported component.xml" }
    connect_bd_intf_net $pin [get_bd_intf_pins memory_xbar/$slot]
}
connect_bd_intf_net [get_bd_intf_pins memory_xbar/M00_AXI] [get_bd_intf_pins $ps/S_AXI_HP0_FPD]
foreach pin {maxihpm0_fpd_aclk saxihp0_fpd_aclk} {
    connect_bd_net [get_bd_pins $ps/pl_clk0] [get_bd_pins $ps/$pin]
}
foreach pin {gemv_0/ap_clk control_xbar/aclk memory_xbar/aclk reset_0/slowest_sync_clk} {
    connect_bd_net [get_bd_pins $ps/pl_clk0] [get_bd_pins $pin]
}
connect_bd_net [get_bd_pins $ps/pl_resetn0] [get_bd_pins reset_inverter/Op1]
connect_bd_net [get_bd_pins reset_inverter/Res] [get_bd_pins reset_0/ext_reset_in]
set reset_high [get_property CONFIG.C_EXT_RESET_HIGH [get_bd_cells reset_0]]
puts "Inferred processor-system-reset external polarity (1=active-high): $reset_high"
if {$reset_high ne "1"} {
    error "Reset inverter produces active-high reset, but proc_sys_reset does not expect C_EXT_RESET_HIGH=1"
}
connect_bd_net [get_bd_pins locked_0/dout] [get_bd_pins reset_0/dcm_locked]
# Active-low auxiliary reset is inactive at one; debug reset is inactive at zero.
connect_bd_net [get_bd_pins locked_0/dout] [get_bd_pins reset_0/aux_reset_in]
connect_bd_net [get_bd_pins zero_0/dout] [get_bd_pins reset_0/mb_debug_sys_rst]
foreach pin {gemv_0/ap_rst_n control_xbar/aresetn memory_xbar/aresetn} {
    connect_bd_net [get_bd_pins reset_0/peripheral_aresetn] [get_bd_pins $pin]
}
connect_bd_net [get_bd_pins gemv_0/interrupt] [get_bd_pins $ps/pl_ps_irq0]
assign_bd_address -offset $base -range 64K \
    -target_address_space [get_bd_addr_spaces $ps/Data] \
    [get_bd_addr_segs gemv_0/s_axi_control/Reg] -force
foreach port {weights metadata activation output_r} {
    assign_bd_address -offset 0x00000000 -range 2G \
        -target_address_space [get_bd_addr_spaces gemv_0/Data_m_axi_$port] \
        [get_bd_addr_segs $ps/SAXIGP2/HP0_DDR_LOW] -force
}
validate_bd_design
# Check inferred properties AND actual nets after propagation, before saving or
# generating any hardware. A mismatch must abort both bd and bitstream stages.
proc require_reset_property {cell property expected} {
    set actual [get_property $property [get_bd_cells $cell]]
    if {$actual ne $expected} {
        error "Post-validation reset check: $cell $property=$actual; expected $expected"
    }
}
proc require_reset_net {source sinks} {
    set source_pin [get_bd_pins $source]
    set net [get_bd_nets -quiet -of_objects $source_pin]
    if {[llength $net] != 1} {
        error "Post-validation reset check: $source must drive exactly one net"
    }
    set drivers [get_bd_pins -quiet -of_objects $net -filter {DIR == O}]
    if {[llength $drivers] != 1 || [lindex $drivers 0] ne $source_pin} {
        error "Post-validation reset check: unexpected drivers on $source net: $drivers"
    }
    foreach sink $sinks {
        set sink_net [get_bd_nets -quiet -of_objects [get_bd_pins $sink]]
        if {$sink_net ne $net} {
            error "Post-validation reset check: $sink is not connected to $source"
        }
    }
}
require_reset_property reset_0 CONFIG.C_EXT_RESET_HIGH 1
require_reset_property reset_0 CONFIG.C_AUX_RESET_HIGH 0
# Constants may be returned as decimal or hexadecimal by Vivado.
foreach {cell expected} {locked_0 1 zero_0 0} {
    set value [get_property CONFIG.CONST_VAL [get_bd_cells $cell]]
    if {![string is wideinteger -strict $value] || $value != $expected} {
        error "Post-validation reset check: $cell constant=$value; expected $expected"
    }
}
require_reset_property reset_inverter CONFIG.C_OPERATION not
require_reset_property reset_inverter CONFIG.C_SIZE 1
require_reset_net $ps/pl_resetn0 {reset_inverter/Op1}
require_reset_net reset_inverter/Res {reset_0/ext_reset_in}
require_reset_net locked_0/dout {reset_0/aux_reset_in reset_0/dcm_locked}
require_reset_net zero_0/dout {reset_0/mb_debug_sys_rst}
require_reset_net reset_0/peripheral_aresetn {gemv_0/ap_rst_n control_xbar/aresetn memory_xbar/aresetn}
puts "Post-validation reset checks PASS: external active-high via inverter; auxiliary active-low tied high; reset outputs connected"
save_bd_design
write_bd_tcl -force "$work/soc_gemv_bd.tcl"
assign_bd_address -export_to_file "$work/address_map.csv" -force
if {$stage eq "bd"} {
    puts "P4.1 block design saved; control base $base. No bitstream generated."
    close_project
    return
}
set bd [get_files */soc_gemv.bd]
generate_target all $bd
set wrapper [make_wrapper -files $bd -top]
add_files -norecurse $wrapper
set_property top soc_gemv_wrapper [current_fileset]
update_compile_order -fileset sources_1
launch_runs impl_1 -to_step write_bitstream -jobs $jobs
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} { error "Implementation failed" }
open_run impl_1
set pl_clocks [get_clocks -of_objects [get_nets -hier -filter {NAME =~ *zynq_ultra_ps_e_0_pl_clk0}]]
if {[llength $pl_clocks] != 1} {
    error "Cannot identify exactly one implemented PS PL0 clock; inspect report_clocks before deployment"
}
set actual_period [get_property PERIOD $pl_clocks]
puts "Implemented PL0 period: $actual_period ns; requested: [expr {1000.0 / $mhz}] ns"
if {abs($actual_period - 1000.0 / $mhz) > 0.05} {
    error "Implemented PL0 clock differs from requested MHz; do not deploy this build"
}
report_clocks -file "$work/clocks.rpt"
report_timing_summary -file "$work/timing_summary.rpt"
report_utilization -file "$work/utilization.rpt"
foreach delay_type {max min} {
    set paths [get_timing_paths -delay_type $delay_type -max_paths 1]
    if {[llength $paths] == 0 || [get_property SLACK [lindex $paths 0]] < 0} {
        error "Timing not met ($delay_type); inspect $work/timing_summary.rpt"
    }
}
set out [file normalize "$here/../../out"]
file mkdir $out
set stem "$out/gemv_$tag"
set hwh "$work/gemv.gen/sources_1/bd/soc_gemv/hw_handoff/soc_gemv.hwh"
if {![file isfile $hwh]} { error "Generated HWH missing: $hwh" }
file copy -force "$work/gemv.runs/impl_1/soc_gemv_wrapper.bit" "$stem.bit"
file copy -force $hwh "$stem.hwh"
write_hw_platform -fixed -include_bit -force -file "$stem.xsa"
puts "P4.1 bitstream/HWH/XSA: $stem; control base $base"
close_project
