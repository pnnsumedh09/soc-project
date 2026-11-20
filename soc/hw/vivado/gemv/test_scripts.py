#!/usr/bin/env python3
"""Mocked Tcl guards/topology/packaging checks; never claims vendor validation."""
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HLS = HERE.parent.parent / "gemv"
MOCK = r'''
set script [lindex $argv 0]
set case [lindex $argv 1]
set env(SOC_GEMV_BUILD_STAGE) check
set created 0
set project_args {}
set connected {}
set connected_nets {}
set offsets {}
set exported 0
set props {}
proc get_board_parts {args} {
    if {[info exists ::env(SOC_GEMV_BOARD_PART)]} {
        set requested [lindex $args end]
        if {$requested eq "*" || $requested eq $::env(SOC_GEMV_BOARD_PART)} {
            return [list $::env(SOC_GEMV_BOARD_PART)]
        }
    }
    return {}
}
proc get_property {name object} {
    if {$name eq "PART_NAME" && [string match "avnet.com:ultra96v2:*" $object]} {
        return xczu3eg-sbva484-1-i
    }
    if {$name eq "PART_NAME" && [string match "example.com:wrong:*" $object]} {
        return xczu3eg-sbva484-2-i
    }
    error "Unexpected property request: $name $object"
}
proc create_project {args} { incr ::created; set ::project_args $args }
proc create_bd_cell {args} { return [lindex $args end] }
foreach cmd {current_project current_fileset get_bd_cells get_bd_intf_pins get_bd_pins get_bd_addr_spaces get_bd_addr_segs} {
    proc $cmd {args} { return [lindex $args end] }
}
proc set_property {args} { lappend ::props $args }
proc connect_bd_intf_net {args} { lappend ::connected $args }
proc connect_bd_net {args} { lappend ::connected_nets $args }
proc assign_bd_address {args} {
    set i [lsearch -exact $args -offset]
    if {$i >= 0} { lappend ::offsets [lindex $args [expr {$i+1}]] }
}
foreach cmd {update_ip_catalog create_bd_design apply_bd_automation validate_bd_design save_bd_design write_bd_tcl close_project} {
    proc $cmd {args} {}
}
proc open_project {args} {
    if {[llength $args] != 1 || [string first / [lindex $args 0]] >= 0} { error "Unexpected reset/absolute project" }
}
proc open_solution {args} { if {$args ne {solution}} { error "Unexpected solution reset" } }
proc export_design {args} { set ::exported 1 }
foreach cmd {csim_design csynth_design cosim_design} {
    proc $cmd {args} { error "Packaging must not rerun simulations/synthesis" }
}
rename exit real_exit
proc exit {args} {}
if {$case eq "bd"} { set env(SOC_GEMV_BUILD_STAGE) bd }
if {$case eq "board_v2"} {
    set env(SOC_GEMV_BUILD_STAGE) bd
    set env(SOC_GEMV_BOARD_PART) avnet.com:ultra96v2:part0:1.2
    unset -nocomplain env(SOC_GEMV_PS_CONFIG)
}
if {$case eq "board_wrong"} {
    set env(SOC_GEMV_BUILD_STAGE) bd
    set env(SOC_GEMV_BOARD_PART) example.com:wrong:part0:1.0
    unset -nocomplain env(SOC_GEMV_PS_CONFIG)
}
if {$case eq "no_ps"} { set env(SOC_GEMV_BUILD_STAGE) bd; unset env(SOC_GEMV_PS_CONFIG) }
if {$case eq "bad_base"} { set env(SOC_GEMV_CONTROL_BASE) 0xa0000001 }
if {$case eq "r8"} { set env(SOC_GEMV_HW_ROW_INTERLEAVE) 8 }
set bad [expr {$case in {no_ps bad_base r8 bad_abi bad_header bad_part bad_clock board_wrong}}]
set failed [catch {source $script} message]
if {$bad} {
    if {!$failed || $created != 0} { error "Guard failed: $case / $message" }
    puts "Guard $case PASS: $message"
} else {
    if {$failed} { error $message }
    if {$case eq "check" && $created != 0} { error "Preflight created a project" }
    if {$case eq "export" && !$exported} { error "Did not package existing RTL" }
    if {$case in {bd board_v2}} {
        if {$created != 1 || [llength $connected] != 7 || $offsets ne {0xA0000000}} {
            error "Unexpected AXI topology/address map: $connected / $offsets"
        }
        if {[lsearch -exact $connected_nets [list gemv_0/interrupt zynq_ultra_ps_e_0/pl_ps_irq0]] < 0} {
            error "Missing GEMV interrupt connection: $connected_nets"
        }
        foreach {port slot} {weights S00_AXI scales S01_AXI activation S02_AXI output_r S03_AXI} {
            if {[lsearch -exact $connected [list gemv_0/m_axi_$port memory_xbar/$slot]] < 0} {
                error "Missing memory interface $port"
            }
        }
        set found 0
        foreach p $props {
            if {[lindex $p 0] eq "-dict" && [lindex $p end] eq "memory_xbar" &&
                [dict get [lindex $p 1] CONFIG.NUM_SI] == 4} { set found 1 }
        }
        if {!$found} { error "Expected four memory masters" }
        if {$case eq "board_v2"} {
            set part_index [lsearch -exact $project_args -part]
            if {$part_index < 0 ||
                [lindex $project_args [expr {$part_index + 1}]] ne "xczu3eg-sbva484-1-i"} {
                error "Ultra96-V2 project did not select the board device: $project_args"
            }
        }
    }
    puts "Stage $case PASS (vendor commands mocked)"
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="soc_gemv_p4_scripts_") as temp:
        root = Path(temp)
        hw = root / "soc/hw"
        vivado = hw / "vivado/gemv"
        hls = hw / "gemv"
        vivado.mkdir(parents=True)
        hls.mkdir()
        shutil.copy(HERE / "build_vivado.tcl", vivado)
        for name in ("check_export.tcl", "export_existing.tcl"):
            shutil.copy(HLS / name, hls)
        solution = hw / "hls_work/gemv/f150_b64_o8/solution"
        report = solution / "syn/report"
        report.mkdir(parents=True)
        (solution / "syn/verilog").mkdir()
        (solution / "syn/verilog/soc_gemv_hw.v").write_text("// mock RTL\n")
        (report / "soc_gemv_hw_csynth.rpt").write_text("soc_gemv_hw_Pipeline_read_words\n")
        xml = "<root><Part>xczu3eg-sbva484-1-e</Part><TargetClockPeriod>6.67</TargetClockPeriod></root>"
        xmlpath = report / "soc_gemv_hw_csynth.xml"
        xmlpath.write_text(xml)
        ip = solution / "impl/ip"
        header = ip / "drivers/soc_gemv_hw/src/xsoc_gemv_hw_hw.h"
        header.parent.mkdir(parents=True)
        # Real Vitis 2022.2 emits both: the old *_hw.h glob matched them both.
        api_header = header.with_name("xsoc_gemv_hw.h")
        api_header.write_text('#include "xsoc_gemv_hw_hw.h"\n')
        (ip / "component.xml").write_text("<mock />\n")
        registers = dict(AP_CTRL=0, GIE=4, W_QUANTS_DATA=0x10, W_SCALES_DATA=0x20,
                         X_NATIVE_DATA=0x30, Y_DATA=0x40, ROWS_DATA=0x50,
                         NB_DATA=0x58, STATUS_DATA=0x60, STATUS_CTRL=0x64)
        abi = "".join(f"#define XSOC_GEMV_HW_CONTROL_ADDR_{name} {value:#x}\n"
                      for name, value in registers.items())
        header.write_text(abi)
        assert len(list(header.parent.glob("*_hw.h"))) == 2
        ps = root / "ps.tcl"
        ps.write_text("# mock verified course PS settings\n")
        mock = root / "mock.tcl"
        mock.write_text(MOCK)
        import os
        env = {k:v for k,v in os.environ.items() if not k.startswith("SOC_GEMV_")}
        env["SOC_GEMV_PS_CONFIG"] = str(ps)
        for case in ("check", "bd", "board_v2", "board_wrong", "no_ps", "bad_base", "r8", "bad_abi", "bad_header", "bad_part", "bad_clock", "export"):
            header.write_text(abi.replace("STATUS_DATA 0x60", "STATUS_DATA 0x68") if case=="bad_abi" else abi)
            if case == "bad_header": header.unlink()
            current_xml = xml.replace("-1-e", "-1-i") if case=="bad_part" else xml
            if case=="bad_clock": current_xml = xml.replace("6.67", "10.00")
            xmlpath.write_text(current_xml)
            script = hls / "export_existing.tcl" if case=="export" else vivado / "build_vivado.tcl"
            proc = subprocess.run(["tclsh", str(mock), str(script), case], env=env,
                                  text=True, capture_output=True)
            if proc.returncode:
                raise RuntimeError(f"{case}: {proc.stdout}\n{proc.stderr}")
            print(proc.stdout.splitlines()[-1])
    print("GEMV Tcl checks: PASS (mocked; Vitis/Vivado not executed)")


if __name__ == "__main__":
    main()
