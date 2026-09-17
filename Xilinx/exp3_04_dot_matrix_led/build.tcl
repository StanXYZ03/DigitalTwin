set script_dir [file dirname [file normalize [info script]]]
set out_dir "$script_dir/output"
file mkdir $out_dir

read_verilog "$script_dir/src/exp3_04_dot_matrix_led_top.v"
read_xdc "$script_dir/constraints/platform.xdc"

synth_design -top exp3_04_dot_matrix_led_top -part xc7a100tfgg484-2
opt_design
place_design
route_design

report_utilization -file "$out_dir/utilization.rpt"
report_timing_summary -file "$out_dir/timing_summary.rpt"
report_drc -file "$out_dir/drc.rpt"

set bit_file "$out_dir/exp3_04_dot_matrix_led.bit"
set bin_file "$out_dir/exp3_04_dot_matrix_led.bin"
write_checkpoint -force "$out_dir/exp3_04_dot_matrix_led_routed.dcp"
write_bitstream -force $bit_file
write_cfgmem -force -format bin -size 16 -interface SPIx1 -loadbit [list up 0x00000000 $bit_file] -file $bin_file

set timing_paths [get_timing_paths -quiet -max_paths 1]
if {[llength $timing_paths] > 0} {
    puts "BUILD_WNS=[get_property SLACK [lindex $timing_paths 0]]"
}
puts "BUILD_BIT=$bit_file"
puts "BUILD_BIN=$bin_file"
puts "BUILD_COMPLETE"
