set script_dir [file dirname [file normalize [info script]]]
set bin_file "$script_dir/output/exp3_04_dot_matrix_led.bin"
set bit_file "$script_dir/output/exp3_04_dot_matrix_led.bit"
set cfgmem_name mt25ql128-spi-x1_x2_x4

if {![file exists $bin_file]} {error "Baseline BIN is missing: $bin_file"}
if {![file exists $bit_file]} {error "Baseline BIT is missing: $bit_file"}

load_features labtools
open_hw
connect_hw_server -url localhost:3121
set target [lindex [get_hw_targets -quiet] 0]
if {$target eq ""} {error "No Xilinx JTAG target detected"}
current_hw_target $target
set_property PARAM.FREQUENCY 6000000 $target
open_hw_target $target
set dev [lindex [get_hw_devices -quiet *xc7a100t*] 0]
if {$dev eq ""} {error "No xc7a100t detected"}
current_hw_device $dev
refresh_hw_device -update_hw_probes false $dev

set cfgmem_part [lindex [get_cfgmem_parts $cfgmem_name] 0]
create_hw_cfgmem -hw_device $dev $cfgmem_part
set cfg [get_property PROGRAM.HW_CFGMEM $dev]
set_property PROGRAM.ADDRESS_RANGE use_file $cfg
set_property PROGRAM.FILES [list $bin_file] $cfg
set_property PROGRAM.PRM_FILE {} $cfg
set_property PROGRAM.UNUSED_PIN_TERMINATION pull-none $cfg
set_property PROGRAM.BLANK_CHECK 0 $cfg
set_property PROGRAM.ERASE 1 $cfg
set_property PROGRAM.CFG_PROGRAM 1 $cfg
set_property PROGRAM.VERIFY 1 $cfg
set_property PROGRAM.CHECKSUM 0 $cfg

create_hw_bitstream -hw_device $dev [get_property PROGRAM.HW_CFGMEM_BITFILE $dev]
program_hw_devices $dev
refresh_hw_device -update_hw_probes false $dev
program_hw_cfgmem -hw_cfgmem $cfg
puts "DOT_BASELINE_FLASH_PROGRAM_SUCCESS"

# Leave the verified experiment running immediately, without requiring a
# cold boot through the still-under-debug external mode-selection path.
set_property PROBES.FILE {} $dev
set_property FULL_PROBES.FILE {} $dev
set_property PROGRAM.FILE $bit_file $dev
program_hw_devices $dev
refresh_hw_device -update_hw_probes false $dev
puts "DOT_BASELINE_SRAM_PROGRAM_SUCCESS"

close_hw_target
disconnect_hw_server
close_hw
exit
