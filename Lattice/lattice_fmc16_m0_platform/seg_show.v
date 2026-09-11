// seg_show.v -- seven-segment scan driver aligned to the board's measured wiring
//
// Physical wiring on board:
//   - sel[7:0] drives segment lines, active-high
//   - heg[7:0] drives digit enables, active-low
//
// Display data convention:
//   - spi_data_reg[3:0]   -> DIG1
//   - spi_data_reg[7:4]   -> DIG2
//   - ...
//   - spi_data_reg[31:28] -> DIG8

module seg_show #(
    parameter SCAN_BLANK_CYCLES = 10'd500
)(
    input         sys_clk,
    input         rst_n,
    input         clk_1k,
    input         mode_reset,
    input  [3:0]  mode,
    input  [3:0]  cnt_shift,
    input  [47:0] spi_data_reg,
    output [7:0]  sel,
    output [7:0]  heg
);

    reg  [2:0]  scan_state;
    reg  [7:0]  digit_sel_reg;
    reg  [7:0]  next_digit_sel_reg;
    reg  [2:0]  next_scan_state;
    reg         scan_blank_active;
    reg  [9:0]  scan_blank_cnt;
    reg         clk_1k_d;
    reg  [7:0]  seg_pattern_raw;
    reg  [2:0]  digit_index;
    reg         blank_digit;

    wire [7:0]  digit_port_raw;
    wire [7:0]  segment_port_raw;
    wire        mode5_raw_direct;
    wire        mode9_raw_direct;

    function [7:0] seg_decode;
        input [3:0] val;
        begin
            case (val)
                4'h0: seg_decode = 8'b11111100;
                4'h1: seg_decode = 8'b01100000;
                4'h2: seg_decode = 8'b11011010;
                4'h3: seg_decode = 8'b11110010;
                4'h4: seg_decode = 8'b01100110;
                4'h5: seg_decode = 8'b10110110;
                4'h6: seg_decode = 8'b10111110;
                4'h7: seg_decode = 8'b11100000;
                4'h8: seg_decode = 8'b11111110;
                4'h9: seg_decode = 8'b11110110;
                4'hA: seg_decode = 8'b11101110;
                4'hB: seg_decode = 8'b00111110;
                4'hC: seg_decode = 8'b10011100;
                4'hD: seg_decode = 8'b01111010;
                4'hE: seg_decode = 8'b10011110;
                4'hF: seg_decode = 8'b10001110;
                default: seg_decode = 8'h00;
            endcase
        end
    endfunction

    function [7:0] seg_to_sel;
        input [7:0] seg_raw;
        begin
            // Logical order is {A,B,C,D,E,F,G,DP}
            // Physical order is sel[7:0]={DP,G,F,E,D,C,B,A}
            seg_to_sel = {seg_raw[0], seg_raw[1], seg_raw[2], seg_raw[3],
                          seg_raw[4], seg_raw[5], seg_raw[6], seg_raw[7]};
        end
    endfunction

    function [3:0] digit_nibble;
        input [47:0] frame;
        input [2:0]  idx;
        begin
            case (idx)
                3'd0:    digit_nibble = frame[3:0];
                3'd1:    digit_nibble = frame[7:4];
                3'd2:    digit_nibble = frame[11:8];
                3'd3:    digit_nibble = frame[15:12];
                3'd4:    digit_nibble = frame[19:16];
                3'd5:    digit_nibble = frame[23:20];
                3'd6:    digit_nibble = frame[27:24];
                default: digit_nibble = frame[31:28];
            endcase
        end
    endfunction

    function [3:0] mode0_nibble;
        input [47:0] frame;
        input [2:0]  idx;
        begin
            case (idx)
                3'd0:    mode0_nibble = frame[31:28];
                3'd1:    mode0_nibble = frame[27:24];
                3'd2:    mode0_nibble = frame[23:20];
                3'd3:    mode0_nibble = frame[19:16];
                3'd4:    mode0_nibble = frame[15:12];
                3'd5:    mode0_nibble = frame[11:8];
                3'd6:    mode0_nibble = frame[7:4];
                default: mode0_nibble = frame[3:0];
            endcase
        end
    endfunction


    assign mode5_raw_direct = (mode == 4'd5);
    assign mode9_raw_direct = (mode == 4'd9);
    assign digit_port_raw   = mode5_raw_direct ? spi_data_reg[31:24] :
                              mode9_raw_direct ? spi_data_reg[31:24] :
                                                 digit_sel_reg;
    assign segment_port_raw = mode5_raw_direct ? seg_to_sel(spi_data_reg[23:16]) :
                              mode9_raw_direct ? seg_to_sel(spi_data_reg[23:16]) :
                                                 seg_to_sel(seg_pattern_raw);

    assign sel = segment_port_raw;
    assign heg = ~digit_port_raw;

    always @(posedge sys_clk or negedge rst_n) begin
        if (!rst_n) begin
            scan_state         <= 3'd0;
            digit_sel_reg      <= 8'h00;
            next_digit_sel_reg <= 8'h00;
            next_scan_state    <= 3'd0;
            scan_blank_active  <= 1'b0;
            scan_blank_cnt     <= 10'd0;
            clk_1k_d           <= 1'b0;
        end else begin
            clk_1k_d <= clk_1k;

            if (mode_reset) begin
                scan_state         <= 3'd0;
                digit_sel_reg      <= 8'h00;
                next_digit_sel_reg <= 8'h00;
                next_scan_state    <= 3'd0;
                scan_blank_active  <= 1'b0;
                scan_blank_cnt     <= 10'd0;
            end else if (~clk_1k_d & clk_1k && !scan_blank_active) begin
                digit_sel_reg     <= 8'h00;
                scan_blank_active <= 1'b1;
                scan_blank_cnt    <= SCAN_BLANK_CYCLES - 10'd1;
                case (scan_state)
                    3'd0:    begin next_digit_sel_reg <= 8'b0000_0001; next_scan_state <= 3'd1; end
                    3'd1:    begin next_digit_sel_reg <= 8'b0000_0010; next_scan_state <= 3'd2; end
                    3'd2:    begin next_digit_sel_reg <= 8'b0000_0100; next_scan_state <= 3'd3; end
                    3'd3:    begin next_digit_sel_reg <= 8'b0000_1000; next_scan_state <= 3'd4; end
                    3'd4:    begin next_digit_sel_reg <= 8'b0001_0000; next_scan_state <= 3'd5; end
                    3'd5:    begin next_digit_sel_reg <= 8'b0010_0000; next_scan_state <= 3'd6; end
                    3'd6:    begin next_digit_sel_reg <= 8'b0100_0000; next_scan_state <= 3'd7; end
                    default: begin next_digit_sel_reg <= 8'b1000_0000; next_scan_state <= 3'd0; end
                endcase
            end else if (scan_blank_active) begin
                if (scan_blank_cnt == 10'd0) begin
                    digit_sel_reg     <= next_digit_sel_reg;
                    scan_state        <= next_scan_state;
                    scan_blank_active <= 1'b0;
                end else begin
                    scan_blank_cnt <= scan_blank_cnt - 10'd1;
                end
            end
        end
    end

    always @(*) begin
        case (digit_sel_reg)
            8'b0000_0001: digit_index = 3'd0;
            8'b0000_0010: digit_index = 3'd1;
            8'b0000_0100: digit_index = 3'd2;
            8'b0000_1000: digit_index = 3'd3;
            8'b0001_0000: digit_index = 3'd4;
            8'b0010_0000: digit_index = 3'd5;
            8'b0100_0000: digit_index = 3'd6;
            8'b1000_0000: digit_index = 3'd7;
            default:      digit_index = 3'd0;
        endcase
    end

    always @(*) begin
        seg_pattern_raw = 8'h00;
        blank_digit     = 1'b0;

        case (mode)
            4'd0:  blank_digit = 1'b0;
            4'd1:  blank_digit = (digit_index < 3'd4);
            4'd4:  blank_digit = 1'b0;
            4'd6:  blank_digit = (digit_index < 3'd4);
            4'd7:  blank_digit = 1'b0;
            4'd11,
            4'd12: blank_digit = 1'b1;
            default: blank_digit = 1'b0;
        endcase

        if (!blank_digit) begin
            case (mode)
                4'd2: begin
                    if (digit_index < 3'd4) begin
                        seg_pattern_raw = seg_decode(digit_nibble(spi_data_reg, digit_index));
                    end else begin
                        case (digit_index)
                            3'd4:    seg_pattern_raw = spi_data_reg[23:16];
                            3'd5:    seg_pattern_raw = spi_data_reg[31:24];
                            3'd6:    seg_pattern_raw = spi_data_reg[39:32];
                            default: seg_pattern_raw = spi_data_reg[47:40];
                        endcase
                    end
                end
                4'd4: begin
                    seg_pattern_raw = seg_decode(digit_nibble(spi_data_reg, digit_index));
                end
                4'd6: begin
                    case (digit_index)
                        3'd4:    seg_pattern_raw = spi_data_reg[23:16];
                        3'd5:    seg_pattern_raw = spi_data_reg[31:24];
                        3'd6:    seg_pattern_raw = spi_data_reg[39:32];
                        default: seg_pattern_raw = spi_data_reg[47:40];
                    endcase
                end
                default: begin
                    if (mode == 4'd0)
                        seg_pattern_raw = seg_decode(mode0_nibble(spi_data_reg, digit_index));
                    else
                        seg_pattern_raw = seg_decode(digit_nibble(spi_data_reg, digit_index));
                end
            endcase
        end
    end

endmodule
