`timescale 1ns/1ps

`ifndef EXP3_04_TOP_MODULE
`define EXP3_04_TOP_MODULE exp3_04_dot_matrix_led_top
`endif

module `EXP3_04_TOP_MODULE #(
    parameter integer SCAN_DIV   = 16'd7812,
    parameter integer SCAN_BLANK_CYCLES = 8'd125,
    parameter integer PI_FILTER_CYCLES = 20'd125000,
    parameter integer FRAME_DIV0 = 26'd31249999,
    parameter integer FRAME_DIV1 = 26'd15624999,
    parameter integer FRAME_DIV2 = 26'd6249999,
    parameter integer FRAME_DIV3 = 26'd3124999
) (
    input  wire        clk_in_p,
    input  wire        clk_in_n,
    input  wire [15:0] PI,
    output wire [31:0] PO,
    output wire [15:0] PIO,
`ifdef PAD_READBACK_DIAG
    inout  wire [15:0] I_ROW,
`else
    output wire [15:0] I_ROW,
`endif
    output wire [3:0]  I_COL
);
    localparam [7:0] EXP_ID = 8'hD4;

    // Use the core-board 125 MHz differential oscillator.  This clock is
    // independent of the bridge board and matches the working counter design.
    wire sys_clk;
    IBUFDS #(
        .DIFF_TERM("FALSE"),
        .IBUF_LOW_PWR("FALSE")
    ) u_sys_clk_ibufds (
        .I (clk_in_p),
        .IB(clk_in_n),
        .O (sys_clk)
    );

    wire       run_latch;
    wire [1:0] mode_latch;
    wire [1:0] speed_latch;
    wire       dir_latch;
    wire       frame_tick;
    wire [7:0] frame_pos;

    wire [15:0] fb_row0;
    wire [15:0] fb_row1;
    wire [15:0] fb_row2;
    wire [15:0] fb_row3;
    wire [15:0] fb_row4;
    wire [15:0] fb_row5;
    wire [15:0] fb_row6;
    wire [15:0] fb_row7;
    wire [15:0] fb_row8;
    wire [15:0] fb_row9;
    wire [15:0] fb_row10;
    wire [15:0] fb_row11;
    wire [15:0] fb_row12;
    wire [15:0] fb_row13;
    wire [15:0] fb_row14;
    wire [15:0] fb_row15;
    wire [3:0]  scan_row;
    wire [15:0] scan_bits;
    wire [15:0] matrix_rows;
    wire [15:0] matrix_drive;
    reg  [3:0]  scan_row_previous = 4'd0;
    reg  [7:0]  scan_heartbeat = 8'd0;
`ifdef PAD_READBACK_DIAG
    reg [26:0] pad_tick = 27'd0;
    reg [7:0] pad_epoch = 8'd0;
    wire [15:0] pad_input;
    reg [15:0] pad_sync1 = 16'd0;
    reg [15:0] pad_sync2 = 16'd0;
    reg [3:0] pad_high = 4'd0;
    reg [3:0] pad_low = 4'd0;
    reg [3:0] pad_released = 4'd0;
    // Drive for only 100 us each second per level, then release.  This
    // limits exposure to possible contention; it cannot guarantee electrical
    // safety or diagnose the downstream mux from FPGA-pad feedback alone.
    wire pad_enable = (pad_tick < 27'd12500) ||
                      ((pad_tick >= 27'd25000) && (pad_tick < 27'd37500));
    wire pad_value = (pad_tick < 27'd12500);
    genvar pad_i;
    generate for (pad_i = 0; pad_i < 16; pad_i = pad_i + 1) begin : pad_buffers
        IOBUF #(.DRIVE(4), .SLEW("SLOW")) u_pad (
            .IO(I_ROW[pad_i]), .I(pad_value), .T(!pad_enable),
            .O(pad_input[pad_i]));
    end endgenerate
    always @(posedge sys_clk) begin
        pad_sync1 <= pad_input;
        pad_sync2 <= pad_sync1;
        if (pad_tick == 27'd124999999) begin
            pad_tick <= 27'd0;
            pad_epoch <= pad_epoch + 1'b1;
        end else pad_tick <= pad_tick + 1'b1;
        // Nibble bits 0..3 correspond to W20,T20,AB21,AB22.
        if (pad_tick == 27'd12000) pad_high <= pad_sync2[7:4];
        if (pad_tick == 27'd37000) pad_low <= pad_sync2[7:4];
        if (pad_tick == 27'd50000) pad_released <= pad_sync2[7:4];
    end
`endif
`ifdef ROW_PATH_DIAG
    reg  [26:0] row_diag_tick = 27'd0;
    reg  [1:0]  row_diag_phase = 2'd0;
    wire [15:0] row_diag_mask = (row_diag_phase == 2'd0) ? 16'hFFFF :
                                (row_diag_phase == 2'd1) ? 16'h3030 :
                                (row_diag_phase == 2'd2) ? 16'h00FF :
                                                                 16'hFF00;

    // Cycle all rows, the originally suspected mask, the low byte and the
    // high byte.  This distinguishes a missing common route from individual
    // row-channel faults without depending on bridge PI keys.
    always @(posedge sys_clk) begin
        if (row_diag_tick == 27'd124999999) begin
            row_diag_tick  <= 27'd0;
            row_diag_phase <= row_diag_phase + 1'b1;
        end else begin
            row_diag_tick <= row_diag_tick + 1'b1;
        end
    end
`endif

    exp3_04_control_engine #(
        .PI_FILTER_CYCLES(PI_FILTER_CYCLES),
        .FRAME_DIV0(FRAME_DIV0),
        .FRAME_DIV1(FRAME_DIV1),
        .FRAME_DIV2(FRAME_DIV2),
        .FRAME_DIV3(FRAME_DIV3)
    ) u_control (
        .sys_clk(sys_clk),
        // Reuse the F1..F4 -> PI7..PI10 path proven by exp2-01.
        .PI(PI[10:7]),
        .run_latch(run_latch),
        .mode_latch(mode_latch),
        .speed_latch(speed_latch),
        .dir_latch(dir_latch),
        .frame_tick(frame_tick),
        .frame_pos(frame_pos)
    );

    exp3_04_pattern_engine u_pattern (
        .sys_clk(sys_clk),
        .frame_tick(frame_tick),
        .run_latch(run_latch),
        .mode_latch(mode_latch),
        .dir_latch(dir_latch),
        .frame_pos(frame_pos),
        .fb_row0(fb_row0),
        .fb_row1(fb_row1),
        .fb_row2(fb_row2),
        .fb_row3(fb_row3),
        .fb_row4(fb_row4),
        .fb_row5(fb_row5),
        .fb_row6(fb_row6),
        .fb_row7(fb_row7),
        .fb_row8(fb_row8),
        .fb_row9(fb_row9),
        .fb_row10(fb_row10),
        .fb_row11(fb_row11),
        .fb_row12(fb_row12),
        .fb_row13(fb_row13),
        .fb_row14(fb_row14),
        .fb_row15(fb_row15)
    );

    exp3_04_scan_engine #(
        .SCAN_DIV(SCAN_DIV),
        .BLANK_CYCLES(SCAN_BLANK_CYCLES)
    ) u_scan (
        .sys_clk(sys_clk),
        .fb_row0(fb_row0),
        .fb_row1(fb_row1),
        .fb_row2(fb_row2),
        .fb_row3(fb_row3),
        .fb_row4(fb_row4),
        .fb_row5(fb_row5),
        .fb_row6(fb_row6),
        .fb_row7(fb_row7),
        .fb_row8(fb_row8),
        .fb_row9(fb_row9),
        .fb_row10(fb_row10),
        .fb_row11(fb_row11),
        .fb_row12(fb_row12),
        .fb_row13(fb_row13),
        .fb_row14(fb_row14),
        .fb_row15(fb_row15),
        .I_ROW(matrix_rows),
        .I_COL(I_COL),
        .scan_row(scan_row),
        .scan_bits(scan_bits)
    );

`ifdef ROW_PATH_DIAG
    assign matrix_drive = row_diag_mask;
`else
    assign matrix_drive = matrix_rows;
`endif
`ifndef PAD_READBACK_DIAG
    assign I_ROW = matrix_drive;
`endif

    // The matrix experiment does not own the general-purpose PIO bus.  Keep
    // it released so that the discrete LEDs are not driven by matrix data.
    assign PIO = 16'hzzzz;

    // Count physical-column transitions.  The high nibble proves that the
    // scan engine is alive even when an FMC poll repeatedly lands on the same
    // column phase; the low nibble reports the current I_COL value.
    always @(posedge sys_clk) begin
        scan_row_previous <= scan_row;
        if (scan_row != scan_row_previous)
            scan_heartbeat <= scan_heartbeat + 1'b1;
    end

    // During hardware bring-up, PO[7:4] is a scan heartbeat and PO[3:0]
    // mirrors I_COL.  Normal experiment state remains in PO[31:8].
`ifdef PAD_READBACK_DIAG
    // Fixed marker, heartbeat, reserved, released/low/high sample nibbles.
    assign PO = {8'hD5, pad_epoch, 4'h0, pad_released, pad_low, pad_high};
`elsif ROW_PATH_DIAG
    // PO[3:0] reads 1, 2, 3, 4 for the four diagnostic phases.
    assign PO = {EXP_ID, run_latch, dir_latch, mode_latch,
                 2'b00, speed_latch, frame_pos,
                 2'b00, row_diag_phase + 1'b1};
`else
    assign PO = {EXP_ID, run_latch, dir_latch, mode_latch,
                 2'b00, speed_latch, frame_pos,
                 scan_heartbeat[7:4], scan_row};
`endif

endmodule

module exp3_04_control_engine #(
    parameter integer PI_FILTER_CYCLES = 20'd125000,
    parameter integer FRAME_DIV0 = 26'd31249999,
    parameter integer FRAME_DIV1 = 26'd15624999,
    parameter integer FRAME_DIV2 = 26'd6249999,
    parameter integer FRAME_DIV3 = 26'd3124999
) (
    input  wire       sys_clk,
    input  wire [3:0] PI,
    output reg        run_latch = 1'b0,
    output reg [1:0]  mode_latch = 2'd0,
    output reg [1:0]  speed_latch = 2'd0,
    output reg        dir_latch = 1'b0,
    output wire       frame_tick,
    output reg [7:0]  frame_pos = 8'd0
);
    reg [3:0] pi_meta = 4'd0;
    reg [3:0] pi_sync = 4'd0;
    reg [3:0] pi_candidate = 4'd0;
    reg [3:0] pi_stable = 4'd0;
    reg [3:0] pi_prev = 4'd0;
    reg [19:0] pi_filter_count = 20'd0;
    reg        pi_filter_pending = 1'b1;
    reg        controls_armed = 1'b0;
    wire [3:0] pi_event = (pi_stable ^ pi_prev) & {4{controls_armed}};

    reg [25:0] frame_cnt = 26'd0;
    reg [25:0] frame_div;

    always @(*) begin
        case (speed_latch)
            2'd0:    frame_div = FRAME_DIV0;
            2'd1:    frame_div = FRAME_DIV1;
            2'd2:    frame_div = FRAME_DIV2;
            default: frame_div = FRAME_DIV3;
        endcase
    end

    assign frame_tick = (frame_cnt >= frame_div);

    always @(posedge sys_clk) begin
        pi_meta <= PI;
        pi_sync <= pi_meta;
        pi_prev <= pi_stable;

        // Import the event semantics already proven by the M0 counter:
        // establish the actual startup baseline, filter the complete vector,
        // then treat either transition of a toggled PI bit as one key event.
        if (pi_sync != pi_candidate) begin
            pi_candidate <= pi_sync;
            pi_filter_count <= 20'd0;
            pi_filter_pending <= 1'b1;
        end else if (pi_filter_pending) begin
            if (pi_filter_count >= PI_FILTER_CYCLES - 1) begin
                pi_filter_count <= 20'd0;
                pi_filter_pending <= 1'b0;
                pi_stable <= pi_candidate;
                if (!controls_armed) begin
                    pi_prev <= pi_candidate;
                    controls_armed <= 1'b1;
                end
            end else begin
                pi_filter_count <= pi_filter_count + 1'b1;
            end
        end

        if (pi_event[0]) run_latch <= ~run_latch;
        if (pi_event[1]) mode_latch <= mode_latch + 1'b1;
        if (pi_event[2]) speed_latch <= speed_latch + 1'b1;
        if (pi_event[3]) dir_latch <= ~dir_latch;

        if (frame_tick) begin
            frame_cnt <= 26'd0;
            if (run_latch) begin
                if (dir_latch)
                    frame_pos <= frame_pos - 1'b1;
                else
                    frame_pos <= frame_pos + 1'b1;
            end
        end else begin
            frame_cnt <= frame_cnt + 1'b1;
        end
    end
endmodule

module exp3_04_pattern_engine (
    input  wire        sys_clk,
    input  wire        frame_tick,
    input  wire        run_latch,
    input  wire [1:0]  mode_latch,
    input  wire        dir_latch,
    input  wire [7:0]  frame_pos,
    output reg [15:0]  fb_row0 = 16'h0001,
    output reg [15:0]  fb_row1 = 16'h0000,
    output reg [15:0]  fb_row2 = 16'h0000,
    output reg [15:0]  fb_row3 = 16'h0000,
    output reg [15:0]  fb_row4 = 16'h0000,
    output reg [15:0]  fb_row5 = 16'h0000,
    output reg [15:0]  fb_row6 = 16'h0000,
    output reg [15:0]  fb_row7 = 16'h0000,
    output reg [15:0]  fb_row8 = 16'h0000,
    output reg [15:0]  fb_row9 = 16'h0000,
    output reg [15:0]  fb_row10 = 16'h0000,
    output reg [15:0]  fb_row11 = 16'h0000,
    output reg [15:0]  fb_row12 = 16'h0000,
    output reg [15:0]  fb_row13 = 16'h0000,
    output reg [15:0]  fb_row14 = 16'h0000,
    output reg [15:0]  fb_row15 = 16'h0000
);
    reg [3:0] bounce_x = 4'd1;
    reg [3:0] bounce_y = 4'd1;
    reg       bounce_dx = 1'b0;
    reg       bounce_dy = 1'b0;
    reg       bounce_y_step = 1'b0;

    reg [3:0] chase_x;
    reg [3:0] chase_y;

    function [15:0] scroll_row;
        input [3:0] row;
        input [3:0] shift;
        reg [15:0] glyph;
        begin
            case (row)
                4'd2: glyph = 16'b0011110001111000;
                4'd3: glyph = 16'b0100001010000100;
                4'd4: glyph = 16'b1001100110011010;
                4'd5: glyph = 16'b1010010110100101;
                4'd6: glyph = 16'b1010010110100101;
                4'd7: glyph = 16'b1001100110011010;
                4'd8: glyph = 16'b1000000110000001;
                4'd9: glyph = 16'b0100001001000010;
                4'd10: glyph = 16'b0011110000111100;
                default: glyph = 16'h0000;
            endcase
            scroll_row = (glyph << shift) | (glyph >> (5'd16 - {1'b0, shift}));
        end
    endfunction

    task clear_frame;
        begin
            fb_row0 <= 16'h0000;
            fb_row1 <= 16'h0000;
            fb_row2 <= 16'h0000;
            fb_row3 <= 16'h0000;
            fb_row4 <= 16'h0000;
            fb_row5 <= 16'h0000;
            fb_row6 <= 16'h0000;
            fb_row7 <= 16'h0000;
            fb_row8 <= 16'h0000;
            fb_row9 <= 16'h0000;
            fb_row10 <= 16'h0000;
            fb_row11 <= 16'h0000;
            fb_row12 <= 16'h0000;
            fb_row13 <= 16'h0000;
            fb_row14 <= 16'h0000;
            fb_row15 <= 16'h0000;
        end
    endtask

    always @(posedge sys_clk) begin
        if (frame_tick || !run_latch) begin
            clear_frame();
            case (mode_latch)
                2'd0: begin
                    case (frame_pos[7:4])
                        4'd0: fb_row0 <= 16'h0001 << frame_pos[3:0];
                        4'd1: fb_row1 <= 16'h0001 << frame_pos[3:0];
                        4'd2: fb_row2 <= 16'h0001 << frame_pos[3:0];
                        4'd3: fb_row3 <= 16'h0001 << frame_pos[3:0];
                        4'd4: fb_row4 <= 16'h0001 << frame_pos[3:0];
                        4'd5: fb_row5 <= 16'h0001 << frame_pos[3:0];
                        4'd6: fb_row6 <= 16'h0001 << frame_pos[3:0];
                        4'd7: fb_row7 <= 16'h0001 << frame_pos[3:0];
                        4'd8: fb_row8 <= 16'h0001 << frame_pos[3:0];
                        4'd9: fb_row9 <= 16'h0001 << frame_pos[3:0];
                        4'd10: fb_row10 <= 16'h0001 << frame_pos[3:0];
                        4'd11: fb_row11 <= 16'h0001 << frame_pos[3:0];
                        4'd12: fb_row12 <= 16'h0001 << frame_pos[3:0];
                        4'd13: fb_row13 <= 16'h0001 << frame_pos[3:0];
                        4'd14: fb_row14 <= 16'h0001 << frame_pos[3:0];
                        default: fb_row15 <= 16'h0001 << frame_pos[3:0];
                    endcase
                end
                2'd1: begin
                    if (run_latch && frame_tick) begin
                        if (!bounce_dx && bounce_x == 4'd15) bounce_dx <= 1'b1;
                        else if (bounce_dx && bounce_x == 4'd0) bounce_dx <= 1'b0;
                        else bounce_x <= bounce_dx ? bounce_x - 1'b1 : bounce_x + 1'b1;

                        bounce_y_step <= ~bounce_y_step;
                        if (bounce_y_step) begin
                            if (!bounce_dy && bounce_y == 4'd15) bounce_dy <= 1'b1;
                            else if (bounce_dy && bounce_y == 4'd0) bounce_dy <= 1'b0;
                            else bounce_y <= bounce_dy ? bounce_y - 1'b1 : bounce_y + 1'b1;
                        end
                    end
                    case (bounce_y)
                        4'd0: fb_row0 <= 16'h0001 << bounce_x;
                        4'd1: fb_row1 <= 16'h0001 << bounce_x;
                        4'd2: fb_row2 <= 16'h0001 << bounce_x;
                        4'd3: fb_row3 <= 16'h0001 << bounce_x;
                        4'd4: fb_row4 <= 16'h0001 << bounce_x;
                        4'd5: fb_row5 <= 16'h0001 << bounce_x;
                        4'd6: fb_row6 <= 16'h0001 << bounce_x;
                        4'd7: fb_row7 <= 16'h0001 << bounce_x;
                        4'd8: fb_row8 <= 16'h0001 << bounce_x;
                        4'd9: fb_row9 <= 16'h0001 << bounce_x;
                        4'd10: fb_row10 <= 16'h0001 << bounce_x;
                        4'd11: fb_row11 <= 16'h0001 << bounce_x;
                        4'd12: fb_row12 <= 16'h0001 << bounce_x;
                        4'd13: fb_row13 <= 16'h0001 << bounce_x;
                        4'd14: fb_row14 <= 16'h0001 << bounce_x;
                        default: fb_row15 <= 16'h0001 << bounce_x;
                    endcase
                end
                2'd2: begin
                    fb_row0 <= 16'hffff;
                    fb_row15 <= 16'hffff;
                    fb_row1 <= 16'h8001;
                    fb_row2 <= 16'h8001;
                    fb_row3 <= 16'h8001;
                    fb_row4 <= 16'h8001;
                    fb_row5 <= 16'h8001;
                    fb_row6 <= 16'h8001;
                    fb_row7 <= 16'h8001;
                    fb_row8 <= 16'h8001;
                    fb_row9 <= 16'h8001;
                    fb_row10 <= 16'h8001;
                    fb_row11 <= 16'h8001;
                    fb_row12 <= 16'h8001;
                    fb_row13 <= 16'h8001;
                    fb_row14 <= 16'h8001;
                    case (frame_pos[5:0])
                        6'd0, 6'd1, 6'd2, 6'd3, 6'd4, 6'd5, 6'd6, 6'd7,
                        6'd8, 6'd9, 6'd10, 6'd11, 6'd12, 6'd13, 6'd14, 6'd15: begin
                            chase_x = frame_pos[3:0];
                            chase_y = 4'd0;
                        end
                        6'd16, 6'd17, 6'd18, 6'd19, 6'd20, 6'd21, 6'd22,
                        6'd23, 6'd24, 6'd25, 6'd26, 6'd27, 6'd28, 6'd29, 6'd30: begin
                            chase_x = 4'd15;
                            chase_y = frame_pos[3:0];
                        end
                        6'd31, 6'd32, 6'd33, 6'd34, 6'd35, 6'd36, 6'd37,
                        6'd38, 6'd39, 6'd40, 6'd41, 6'd42, 6'd43, 6'd44, 6'd45: begin
                            chase_x = 4'd15 - frame_pos[3:0];
                            chase_y = 4'd15;
                        end
                        default: begin
                            chase_x = 4'd0;
                            chase_y = 4'd15 - frame_pos[3:0];
                        end
                    endcase
                    case (chase_y)
                        4'd0: fb_row0 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd1: fb_row1 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd2: fb_row2 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd3: fb_row3 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd4: fb_row4 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd5: fb_row5 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd6: fb_row6 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd7: fb_row7 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd8: fb_row8 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd9: fb_row9 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd10: fb_row10 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd11: fb_row11 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd12: fb_row12 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd13: fb_row13 <= (16'h8001 | (16'h0001 << chase_x));
                        4'd14: fb_row14 <= (16'h8001 | (16'h0001 << chase_x));
                        default: fb_row15 <= (16'h8001 | (16'h0001 << chase_x));
                    endcase
                end
                default: begin
                    fb_row0 <= 16'h0000;
                    fb_row1 <= 16'h0000;
                    fb_row2 <= scroll_row(4'd2, frame_pos[3:0]);
                    fb_row3 <= scroll_row(4'd3, frame_pos[3:0]);
                    fb_row4 <= scroll_row(4'd4, frame_pos[3:0]);
                    fb_row5 <= scroll_row(4'd5, frame_pos[3:0]);
                    fb_row6 <= scroll_row(4'd6, frame_pos[3:0]);
                    fb_row7 <= scroll_row(4'd7, frame_pos[3:0]);
                    fb_row8 <= scroll_row(4'd8, frame_pos[3:0]);
                    fb_row9 <= scroll_row(4'd9, frame_pos[3:0]);
                    fb_row10 <= scroll_row(4'd10, frame_pos[3:0]);
                    fb_row11 <= 16'h0000;
                    fb_row12 <= 16'h0000;
                    fb_row13 <= dir_latch ? 16'h00ff : 16'hff00;
                    fb_row14 <= 16'h0000;
                    fb_row15 <= 16'h0000;
                end
            endcase
        end
    end
endmodule

module exp3_04_scan_engine #(
    parameter integer SCAN_DIV = 16'd7812,
    parameter integer BLANK_CYCLES = 8'd125
) (
    input  wire        sys_clk,
    input  wire [15:0] fb_row0,
    input  wire [15:0] fb_row1,
    input  wire [15:0] fb_row2,
    input  wire [15:0] fb_row3,
    input  wire [15:0] fb_row4,
    input  wire [15:0] fb_row5,
    input  wire [15:0] fb_row6,
    input  wire [15:0] fb_row7,
    input  wire [15:0] fb_row8,
    input  wire [15:0] fb_row9,
    input  wire [15:0] fb_row10,
    input  wire [15:0] fb_row11,
    input  wire [15:0] fb_row12,
    input  wire [15:0] fb_row13,
    input  wire [15:0] fb_row14,
    input  wire [15:0] fb_row15,
    output wire [15:0] I_ROW,
    output wire [3:0]  I_COL,
    output wire [3:0]  scan_row,
    output wire [15:0] scan_bits
);
    reg [15:0] scan_cnt = 16'd0;
    reg [3:0] physical_col = 4'd0;
    reg [7:0] blank_cnt = 8'd0;
    reg [15:0] row_output = 16'h0000;
    reg [1:0] scan_phase = 2'd0;
    wire [15:0] remapped_row_bits;
    localparam [1:0] PHASE_DISPLAY    = 2'd0;
    localparam [1:0] PHASE_PRE_BLANK  = 2'd1;
    localparam [1:0] PHASE_POST_BLANK = 2'd2;

    function framebuffer_bit;
        input [3:0] logical_y;
        input [3:0] logical_x;
        begin
            case (logical_y)
                4'd0: framebuffer_bit = fb_row0[logical_x];
                4'd1: framebuffer_bit = fb_row1[logical_x];
                4'd2: framebuffer_bit = fb_row2[logical_x];
                4'd3: framebuffer_bit = fb_row3[logical_x];
                4'd4: framebuffer_bit = fb_row4[logical_x];
                4'd5: framebuffer_bit = fb_row5[logical_x];
                4'd6: framebuffer_bit = fb_row6[logical_x];
                4'd7: framebuffer_bit = fb_row7[logical_x];
                4'd8: framebuffer_bit = fb_row8[logical_x];
                4'd9: framebuffer_bit = fb_row9[logical_x];
                4'd10: framebuffer_bit = fb_row10[logical_x];
                4'd11: framebuffer_bit = fb_row11[logical_x];
                4'd12: framebuffer_bit = fb_row12[logical_x];
                4'd13: framebuffer_bit = fb_row13[logical_x];
                4'd14: framebuffer_bit = fb_row14[logical_x];
                default: framebuffer_bit = fb_row15[logical_x];
            endcase
        end
    endfunction

    genvar physical_row;
    generate
        for (physical_row = 0; physical_row < 16; physical_row = physical_row + 1) begin : gen_physical_row_remap
            localparam [3:0] physical_row_index = physical_row;
            wire [3:0] logical_x = {physical_row_index[3], physical_col[2:0]};
            wire [3:0] logical_y = {physical_col[3], physical_row_index[2:0]};
            assign remapped_row_bits[physical_row] = framebuffer_bit(logical_y, logical_x);
        end
    endgenerate

    always @(posedge sys_clk) begin
        case (scan_phase)
            PHASE_DISPLAY: begin
                row_output <= remapped_row_bits;
                if (scan_cnt >= SCAN_DIV) begin
                    scan_cnt <= 16'd0;
                    row_output <= 16'h0000;
                    blank_cnt <= BLANK_CYCLES;
                    scan_phase <= PHASE_PRE_BLANK;
                end else begin
                    scan_cnt <= scan_cnt + 1'b1;
                end
            end
            PHASE_PRE_BLANK: begin
                row_output <= 16'h0000;
                if (blank_cnt == 8'd0) begin
                    physical_col <= physical_col + 1'b1;
                    blank_cnt <= BLANK_CYCLES;
                    scan_phase <= PHASE_POST_BLANK;
                end else begin
                    blank_cnt <= blank_cnt - 1'b1;
                end
            end
            default: begin
                row_output <= 16'h0000;
                if (blank_cnt == 8'd0) begin
                    row_output <= remapped_row_bits;
                    scan_phase <= PHASE_DISPLAY;
                end else begin
                    blank_cnt <= blank_cnt - 1'b1;
                end
            end
        endcase
    end

    assign scan_row = physical_col;
    assign I_COL = physical_col;
    assign I_ROW = row_output;
    assign scan_bits = row_output;
endmodule
