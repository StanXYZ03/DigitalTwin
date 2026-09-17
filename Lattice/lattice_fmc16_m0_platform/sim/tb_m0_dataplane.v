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

    // M11 forwards a held level and does not mutate the legacy toggle word.
    dut.key_events_armed=1'b1;
    dut.platform_mode=4'd11;
    key_in_m0[0]<=1'b0;
    repeat(5) @(posedge sys_clk); #1;
    if(dut.key_stable_n[0]!==1'b0) $fatal(1,"M11 F1 press debounce failed");
    if(dut.pi_requested!==16'h0000) $fatal(1,"M11 unexpectedly toggled legacy PI");
    key_in_m0[0]<=1'b1;
    repeat(5) @(posedge sys_clk); #1;
    if(dut.key_stable_n[0]!==1'b1) $fatal(1,"M11 F1 release debounce failed");

    // M0 retains one-toggle-per-press event semantics.
    dut.platform_mode=4'd0;
    key_in_m0[0]<=1'b0;
    repeat(5) @(posedge sys_clk); #1;
    if(dut.pi_requested[7]!==1'b1) $fatal(1,"M0 F1 toggle mapping failed");
    $display("PASS: M0/M11 PI semantics, PO/PIO/LED/display and FMC high-Z");
    $finish;
  end
endmodule
