set script_dir [file dirname [file normalize [info script]]]
set bit_file "$script_dir/output/exp3_04_dot_matrix_led.bit"

if {![file exists $bit_file]} {
    error "Bitstream is missing: $bit_file"
}

open_hw
connect_hw_server -url localhost:3121
set targets [get_hw_targets -quiet]
if {[llength $targets] != 1} {
    error "Expected exactly one JTAG target, found [llength $targets]: $targets"
}
set target [lindex $targets 0]
set_property PARAM.FREQUENCY 6000000 $target
open_hw_target $target

set devs [get_hw_devices -quiet xc7a100t_0]
if {[llength $devs] != 1} {
    error "Expected exactly one xc7a100t_0 device, found [llength $devs]: $devs"
}
set dev [lindex $devs 0]
current_hw_device $dev
refresh_hw_device -update_hw_probes false $dev
set_property PROBES.FILE {} $dev
set_property FULL_PROBES.FILE {} $dev
set_property PROGRAM.FILE $bit_file $dev
program_hw_devices $dev
refresh_hw_device -update_hw_probes false $dev
puts "HW_DEVICE=[get_property PART $dev]"
puts "PROGRAM_FILE=[get_property PROGRAM.FILE $dev]"
puts "SRAM_PROGRAM_COMPLETE"

close_hw_target
disconnect_hw_server
close_hw
