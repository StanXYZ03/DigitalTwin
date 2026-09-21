`timescale 1ns/1ps
module tb_m0_dataplane;
  reg sys_clk=0, rst_n=0;
  reg [31:0] po=0;
  reg [15:0] pio_driver=0;
  reg pio_enable=1;
  reg [9:0] key_in_m0=10'h3ff;
  wire [15:0] pio;
  wire [11:0] led;
  wire [7:0] sel, heg;
  tri [15:0] fmc_db;
  reg ne1_n=1, noe_n=1, nwe_n=1;
  integer matrix_col;
  assign pio = pio_enable ? pio_driver : 16'hzzzz;

  fmc16_link_test_top #(.DISPLAY_CLK_DIVIDER(4), .DISPLAY_SCAN_BLANK_CYCLES(2),
                        .KEY_DEBOUNCE_CYCLES(2)) dut (
    .sys_clk(sys_clk), .rst_n(rst_n), .PO(po), .PIO(pio), .LED_out(led),
    .key_in_m0(key_in_m0),
    .sel(sel), .heg(heg), .fmc_db(fmc_db), .fmc_ne1_n(ne1_n),
    .fmc_noe_n(noe_n), .fmc_nwe_n(nwe_n), .fmc_clk_unused(1'b0),
    .fmc_nwait_unused(1'b1)
  );
  always #10 sys_clk=~sys_clk;

  initial begin
    repeat(3) @(posedge sys_clk); rst_n<=1;
    po<=32'h1234_ABCD; pio_driver<=16'h05A3;
    repeat(8) @(posedge sys_clk); #1;
    if(dut.po_coherent!==32'h1234_ABCD) $fatal(1,"PO coherent sample failed");
    if(dut.pio_coherent!==16'h05A3) $fatal(1,"PIO coherent sample failed");
    if(dut.display_frame!==48'h0000_1234_ABCD) $fatal(1,"display frame failed");
    if(led!==12'hA5C) $fatal(1,"LED mapping failed");
    if(pio!==16'h05A3) $fatal(1,"PIO is not released by XO2");
    if(fmc_db!==16'hzzzz) $fatal(1,"FMC DB not high-Z while NE1 inactive");
    ne1_n<=0; nwe_n<=0; noe_n<=1; repeat(2) @(posedge sys_clk); #1;
    if(fmc_db!==16'hzzzz) $fatal(1,"FMC DB not high-Z during write");

    // M11 preserves the first hardware-proven F1 -> PI7 toggle mapping.
    dut.key_events_armed=1'b1;
    dut.platform_mode=4'd11;
    key_in_m0[0]<=1'b0;
    repeat(5) @(posedge sys_clk); #1;
    if(dut.key_stable_n[0]!==1'b0) $fatal(1,"M11 F1 press debounce failed");
    if(dut.pi_requested!==16'h0080) $fatal(1,"M11 F1 toggle mapping failed");
    key_in_m0[0]<=1'b1;
    repeat(5) @(posedge sys_clk); #1;
    if(dut.key_stable_n[0]!==1'b1) $fatal(1,"M11 F1 release debounce failed");

    // F10 remains a transparent experiment input on PI6 and must not change
    // the selected profile.
    key_in_m0[9]<=1'b0;
    repeat(5) @(posedge sys_clk); #1;
    if(dut.pi_requested[6]!==1'b1) $fatal(1,"F10 -> PI6 toggle mapping failed");
    if(dut.platform_mode!==4'd11) $fatal(1,"F10 illegally changed platform mode");
    key_in_m0[9]<=1'b1;
    repeat(5) @(posedge sys_clk); #1;

    // An explicit FMC mode change may select the external route, but it must
    // preserve all latched PI experiment inputs.
    dut.mode_command_value=4'd0;
    dut.mode_command_toggle=~dut.mode_command_seen;
    repeat(2) @(posedge sys_clk); #1;
    if(dut.platform_mode!==4'd0) $fatal(1,"FMC mode command was not applied");
    if(dut.pi_requested!==16'h00C0) $fatal(1,"mode command cleared PI state");

    // The raw observer must wait past column blanking, capture all physical
    // row words and publish only a complete atomic 16-column frame.
    dut.platform_mode=4'd11;
    force dut.build_active=1'b0;
    force dut.tx_valid=1'b0;
    pio_driver<=16'h8000; po<=32'hD4A1_000F;
    repeat(120) @(posedge sys_clk);
    for(matrix_col=0;matrix_col<15;matrix_col=matrix_col+1) begin
      pio_driver <= (16'h0001 << matrix_col);
      po <= {16'hD4A1,12'h000,matrix_col[3:0]};
      repeat(120) @(posedge sys_clk);
    end
    #1;
    if(dut.matrix_snapshot_valid_columns!==16'hFFFF)
      $fatal(1,"matrix frame did not become complete");
    for(matrix_col=0;matrix_col<16;matrix_col=matrix_col+1)
      if(dut.matrix_snapshot[matrix_col] !== (16'h0001 << matrix_col))
        $fatal(1,"matrix column %0d mismatch",matrix_col);
    if(dut.matrix_frame_sequence===32'd0)
      $fatal(1,"matrix frame sequence did not advance");
    release dut.build_active;
    release dut.tx_valid;

    // M0 uses the same one-toggle-per-press event semantics.
    dut.platform_mode=4'd0;
    key_in_m0[0]<=1'b0;
    repeat(5) @(posedge sys_clk); #1;
    if(dut.pi_requested[7]!==1'b0) $fatal(1,"M0 F1 toggle mapping failed");
    $display("PASS: F1-F10 transparent PI mapping, mode-state preservation, raw matrix capture, PO/PIO/LED/display and FMC high-Z");
    $finish;
  end
endmodule
