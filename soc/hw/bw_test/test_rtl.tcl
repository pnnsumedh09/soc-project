set here [file dirname [file normalize [info script]]]
set work [file normalize "$here/../vivado/bw_test_rtl"]
create_project -force bw_test_rtl $work -part xczu3eg-sbva484-1-e
add_files "$here/read-window.v"
add_files -fileset sim_1 "$here/tb-read-window.v"
set_property top tb_read_window [get_filesets sim_1]
set_property xsim.simulate.runtime all [get_filesets sim_1]
launch_simulation
close_sim
close_project
