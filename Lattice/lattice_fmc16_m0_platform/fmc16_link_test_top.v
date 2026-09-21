`timescale 1ns/1ps
// Resource-small FMC16 M0 personality.
//
// This top keeps the proven mini FMC packet link and adds only the M0 data
// plane extracted from xo2_mode_core: coherent PO/PIO sampling, PO display,
// PIO LEDs, and GET_STATUS_SNAPSHOT. The large generic transaction, event,
// cache, and multi-mode engines from fmc16_slave_top are intentionally absent.
module fmc16_link_test_top #(
    parameter [31:0] BUILD_ID = 32'h4C4B_0009,
    parameter integer DISPLAY_CLK_DIVIDER = 25_000,
    parameter [9:0] DISPLAY_SCAN_BLANK_CYCLES = 10'd500,
    parameter integer KEY_DEBOUNCE_CYCLES = 1_000_000
)(
    input  wire        sys_clk,
    input  wire        rst_n,
    input  wire [31:0] PO,
    inout  wire [15:0] PIO,
    input  wire [9:0]  key_in_m0,
    output wire [11:0] LED_out,
    output wire [7:0]  sel,
    output wire [7:0]  heg,
    inout  wire [15:0] fmc_db,
    input  wire        fmc_ne1_n,
    input  wire        fmc_noe_n,
    input  wire        fmc_nwe_n,
    input  wire        fmc_clk_unused,
    input  wire        fmc_nwait_unused
);

    // Keep these physical inputs explicit: FMC_CLK/NWAIT are deliberately
    // unused in the asynchronous protocol, and NWAIT is never driven.
    // Preserve these input pads even though M0 does not use synchronous FMC
    // clocking or NWAIT. They remain input-only and can never contend on FMC.
    (* syn_preserve = 1 *) reg fmc_clk_keep;
    (* syn_preserve = 1 *) reg fmc_nwait_keep;

    // M0 is input-only on PIO. Explicitly release every bit so the experiment
    // FPGA owns the bus and the XO2 only observes it.
    assign PIO = 16'hzzzz;

    reg [31:0] po_meta;
    reg [31:0] po_sample;
    reg [31:0] po_previous;
    reg [31:0] po_coherent;
    reg [15:0] pio_meta;
    reg [15:0] pio_sample;
    reg [15:0] pio_previous;
    reg [15:0] pio_coherent;
    reg [15:0] ms_divider;
    reg [31:0] timestamp_ms;

    // M11 raw matrix observer.  The XC7A platform wrapper mirrors the exact
    // physical I_ROW drive on PIO and I_COL on PO[3:0], tagged by D4A1.
    // Capture only after the column blanking interval.  A response freezes
    // capture while its words are copied into the FMC packet, so STM32 never
    // sees a buffer being changed underneath the serializer.
    reg [15:0] matrix_snapshot [0:15];
    reg [15:0] matrix_valid_columns;
    reg [15:0] matrix_snapshot_valid_columns;
    reg [31:0] matrix_frame_sequence;
    reg [31:0] matrix_capture_timestamp_ms;
    reg [3:0]  matrix_column_previous;
    reg [7:0]  matrix_settle_count;
    reg        tx_valid;
    reg        build_active;
    integer matrix_index;
    wire matrix_observer_present = (po_coherent[31:16] == 16'hD4A1);

    // Panel F1..F10 are active-low and are reported in physical label order.
    // Preserve the first, hardware-verified mapping in every profile:
    // F1..F9 toggle PI7..PI15 and F10 toggles PI6.  Keep this mapping
    // identical in every profile; experiment-mode selection is an FMC-only
    // platform command and must never consume a student's physical key.
    // The XO2 does not drive PI: it only publishes the
    // debounced requested value through FMC16, and the STM32 remains the
    // sole owner of the MCP23017 which physically drives XC7A PI[15:0].
    reg [9:0] key_meta_n;
    reg [9:0] key_sync_n;
    reg [9:0] key_stable_n;
    reg [19:0] key_debounce_count [0:9];
    reg [15:0] pi_requested;
    reg [15:0] key_event_count;
    reg [21:0] key_release_count;
    reg key_events_armed;
    integer key_index;

    // Safe two-profile selector: M0 counter and M11/MB dot-matrix.  FMC
    // opcode 0x0004 selects the profile directly; all ten physical keys stay
    // transparent experiment inputs.
    reg [3:0] platform_mode;
    reg [3:0] mode_command_value;
    reg       mode_command_toggle;
    reg       mode_command_seen;

    // Do not turn a power-up level or contact settling into an experiment
    // input.  Physical key events become eligible only after every key has
    // been observed released continuously for 50 ms.
    localparam [21:0] KEY_RELEASE_ARM_CYCLES = 22'd2_500_000;

    wire po_valid  = (po_sample == po_previous);
    wire pio_valid = (pio_sample == pio_previous);
    wire clk_1k;
    wire [47:0] display_frame;

    always @(posedge sys_clk or negedge rst_n) begin
        if (!rst_n) begin
            po_meta       <= 32'd0;
            po_sample     <= 32'd0;
            po_previous   <= 32'd0;
            po_coherent   <= 32'd0;
            pio_meta      <= 16'd0;
            pio_sample    <= 16'd0;
            pio_previous  <= 16'd0;
            pio_coherent  <= 16'd0;
            ms_divider    <= 16'd0;
            timestamp_ms  <= 32'd0;
            fmc_clk_keep   <= 1'b0;
            fmc_nwait_keep <= 1'b1;
            key_meta_n     <= 10'h3FF;
            key_sync_n     <= 10'h3FF;
            key_stable_n   <= 10'h3FF;
            pi_requested   <= 16'h0000;
            key_event_count <= 16'h0000;
            platform_mode    <= 4'd0;
            mode_command_seen <= 1'b0;
            key_release_count <= 22'd0;
            key_events_armed <= 1'b0;
            matrix_valid_columns <= 16'h0000;
            matrix_snapshot_valid_columns <= 16'h0000;
            matrix_frame_sequence <= 32'd0;
            matrix_capture_timestamp_ms <= 32'd0;
            matrix_column_previous <= 4'd0;
            matrix_settle_count <= 8'd0;
            for (key_index = 0; key_index < 10; key_index = key_index + 1)
                key_debounce_count[key_index] <= 20'd0;
            for (matrix_index = 0; matrix_index < 16; matrix_index = matrix_index + 1) begin
                matrix_snapshot[matrix_index] <= 16'h0000;
            end
        end else begin
            fmc_clk_keep   <= fmc_clk_unused;
            fmc_nwait_keep <= fmc_nwait_unused;
            po_meta        <= PO;
            po_sample    <= po_meta;
            po_previous  <= po_sample;
            pio_meta     <= PIO;
            pio_sample   <= pio_meta;
            pio_previous <= pio_sample;
            key_meta_n   <= key_in_m0;
            key_sync_n   <= key_meta_n;

            if (mode_command_seen != mode_command_toggle) begin
                platform_mode     <= mode_command_value;
                mode_command_seen <= mode_command_toggle;
            end

            if (!key_events_armed) begin
                key_stable_n <= 10'h3FF;
                for (key_index = 0; key_index < 10; key_index = key_index + 1)
                    key_debounce_count[key_index] <= 20'd0;
                if (key_sync_n == 10'h3FF) begin
                    if (key_release_count == KEY_RELEASE_ARM_CYCLES - 1'b1) begin
                        key_release_count <= 22'd0;
                        key_events_armed <= 1'b1;
                    end else begin
                        key_release_count <= key_release_count + 1'b1;
                    end
                end else begin
                    key_release_count <= 22'd0;
                end
            end else begin
                for (key_index = 0; key_index < 10; key_index = key_index + 1) begin
                    if (key_sync_n[key_index] == key_stable_n[key_index]) begin
                        key_debounce_count[key_index] <= 20'd0;
                    end else if (key_debounce_count[key_index] == KEY_DEBOUNCE_CYCLES - 1) begin
                        key_debounce_count[key_index] <= 20'd0;
                        key_stable_n[key_index] <= key_sync_n[key_index];
                        if (!key_sync_n[key_index]) begin
                            if (key_index <= 8)
                                pi_requested[7 + key_index] <= ~pi_requested[7 + key_index];
                            else
                                pi_requested[6] <= ~pi_requested[6];
                            key_event_count <= key_event_count + 16'd1;
                        end
                    end else begin
                        key_debounce_count[key_index] <= key_debounce_count[key_index] + 20'd1;
                    end
                end
            end

            if (po_valid)
                po_coherent <= po_sample;
            if (pio_valid)
                pio_coherent <= pio_sample;

            if ((platform_mode != 4'd11) || !matrix_observer_present) begin
                matrix_valid_columns <= 16'h0000;
                matrix_settle_count <= 8'd0;
                matrix_column_previous <= po_coherent[3:0];
            end else if (build_active || tx_valid) begin
                matrix_valid_columns <= 16'h0000;
                matrix_settle_count <= 8'd0;
                matrix_column_previous <= po_coherent[3:0];
            end else if (po_coherent[3:0] != matrix_column_previous) begin
                matrix_column_previous <= po_coherent[3:0];
                // 100 cycles at 50 MHz exceeds the XC7A's 1 us column blank.
                matrix_settle_count <= 8'd100;
            end else if (matrix_settle_count != 8'd0) begin
                matrix_settle_count <= matrix_settle_count - 1'b1;
                if (matrix_settle_count == 8'd1) begin
                    matrix_snapshot[matrix_column_previous] <= pio_coherent;
                    if ((matrix_valid_columns |
                         (16'h0001 << matrix_column_previous)) == 16'hFFFF) begin
                        matrix_snapshot_valid_columns <= 16'hFFFF;
                        matrix_frame_sequence <= matrix_frame_sequence + 1'b1;
                        matrix_capture_timestamp_ms <= timestamp_ms;
                        matrix_valid_columns <= 16'h0000;
                    end else begin
                        matrix_valid_columns <= matrix_valid_columns |
                                                (16'h0001 << matrix_column_previous);
                    end
                end
            end

            if (ms_divider == 16'd49_999) begin
                ms_divider   <= 16'd0;
                timestamp_ms <= timestamp_ms + 32'd1;
            end else begin
                ms_divider <= ms_divider + 16'd1;
            end
        end
    end

    // Keep the useful display boundary rules from the full 4000HC core.  The
    // reduced FMC build never drives PIO in any mode; XC7A remains its sole
    // owner, which avoids reintroducing the historical bus-contention bug.
    assign display_frame = (platform_mode == 4'd2) ? {po_coherent, pio_coherent} :
                           (platform_mode == 4'd5 || platform_mode == 4'd9) ?
                               {16'h0000, po_coherent[15:0], 16'h0000} :
                           (platform_mode == 4'd6) ? {po_coherent, 16'h0000} :
                               {16'h0000, po_coherent};
    assign LED_out = (platform_mode == 4'd11) ? 12'hFFF :
                     (platform_mode == 4'd1) ? ~po_coherent[27:16] :
                                               ~pio_coherent[11:0];

    clk_divide #(
        .DIVIDER(DISPLAY_CLK_DIVIDER)
    ) u_display_clk (
        .sys_clk (sys_clk),
        .rst_n   (rst_n),
        .clk_1k  (clk_1k)
    );

    seg_show #(
        .SCAN_BLANK_CYCLES(DISPLAY_SCAN_BLANK_CYCLES)
    ) u_display (
        .sys_clk      (sys_clk),
        .rst_n        (rst_n),
        .clk_1k        (clk_1k),
        .mode_reset   (mode_command_seen != mode_command_toggle),
        .mode         (platform_mode),
        .cnt_shift    (4'd0),
        .spi_data_reg (display_frame),
        .sel          (sel),
        .heg          (heg)
    );

    wire        rx_word_valid;
    wire [15:0] rx_word;
    wire        rx_abort;
    wire [15:0] tx_word;
    wire        tx_have_word;
    wire        tx_advance;

    fmc16_phy_async #(
        .WRITE_MIN_CYCLES(2)
    ) u_phy (
        .sys_clk       (sys_clk),
        .rst_n         (rst_n),
        .fmc_ne1_n     (fmc_ne1_n),
        .fmc_noe_n     (fmc_noe_n),
        .fmc_nwe_n     (fmc_nwe_n),
        .fmc_db        (fmc_db),
        .rx_word_valid (rx_word_valid),
        .rx_word       (rx_word),
        .rx_abort      (rx_abort),
        .tx_word       (tx_word),
        .tx_have_word  (tx_have_word),
        .tx_advance    (tx_advance)
    );

    localparam [2:0] K_RESET = 3'd0,
                     K_HELLO = 3'd1,
                     K_INFO  = 3'd2,
                     K_ACK   = 3'd3,
                     K_RESP  = 3'd4,
                     K_NACK  = 3'd5;

    localparam [15:0] CAPABILITIES = 16'h0007; // async R/W + legacy status snapshot

    reg [15:0] tx_mem [0:31];
    reg [6:0]  tx_count;
    reg [6:0]  tx_ptr;

    reg [2:0]  build_kind;
    reg [6:0]  build_total;
    reg [6:0]  build_idx;
    // Two-phase packet construction: prime build_word_q first, then consume
    // it on the following clock for tx_mem and CRC updates.  Keeping the
    // decoder and CRC in separate register-to-register paths removes the
    // build_idx -> decoder -> CRC timing chain.
    reg [15:0] build_word_q;
    reg        build_word_valid;
    reg [15:0] build_crc16;
    reg [31:0] build_crc32;
    reg [31:0] build_seq;
    reg [31:0] build_tid;
    reg [15:0] build_opcode;
    reg [15:0] build_status;
    reg [15:0] build_detail;
    reg [31:0] tx_seq;

    reg        pending_ack;
    reg        pending_response;
    reg        pending_nack;
    reg [31:0] pending_tid;
    reg [15:0] pending_opcode;
    reg [15:0] pending_status;
    reg [15:0] pending_detail;

    function [15:0] crc16_next;
        input [15:0] data_word;
        input [15:0] crc_in;
        integer i;
        reg [15:0] c;
        begin
            c = crc_in;
            for (i = 15; i >= 0; i = i - 1) begin
                if (c[15] ^ data_word[i])
                    c = {c[14:0], 1'b0} ^ 16'h1021;
                else
                    c = {c[14:0], 1'b0};
            end
            crc16_next = c;
        end
    endfunction

    function [31:0] crc32_next;
        input [15:0] data_word;
        input [31:0] crc_in;
        integer i;
        reg [31:0] c;
        begin
            c = crc_in;
            for (i = 15; i >= 0; i = i - 1) begin
                if (c[31] ^ data_word[i])
                    c = {c[30:0], 1'b0} ^ 32'h04C11DB7;
                else
                    c = {c[30:0], 1'b0};
            end
            crc32_next = c;
        end
    endfunction

    function [6:0] total_for_kind;
        input [2:0] k;
        begin
            case (k)
                K_RESET: total_for_kind = 7'd23;
                K_HELLO: total_for_kind = 7'd26;
                K_INFO : total_for_kind = 7'd31;
                default: total_for_kind = 7'd19;
            endcase
        end
    endfunction

    function [7:0] type_for_kind;
        input [2:0] k;
        begin
            case (k)
                K_RESET: type_for_kind = 8'h7F;
                K_HELLO: type_for_kind = 8'h01;
                K_INFO : type_for_kind = 8'h05;
                K_ACK  : type_for_kind = 8'h03;
                K_RESP : type_for_kind = 8'h31;
                K_NACK : type_for_kind = 8'h04;
                default: type_for_kind = 8'h00;
            endcase
        end
    endfunction

    function [4:0] payload_words_for_kind;
        input [2:0] k;
        begin
            case (k)
                K_RESET: payload_words_for_kind = 5'd8;
                K_HELLO: payload_words_for_kind = 5'd11;
                K_INFO : payload_words_for_kind = 5'd16;
                K_RESP : payload_words_for_kind = (build_opcode == 16'h0005) ? 5'd17 :
                                                  (build_opcode == 16'h0003) ? 5'd14 : 5'd10;
                default: payload_words_for_kind = 5'd4;
            endcase
        end
    endfunction

    function [15:0] body_word;
        input [2:0] k;
        input [6:0] pidx;
        begin
            body_word = 16'h0000;
            case (k)
                K_RESET: begin
                    case (pidx)
                        7'd0: body_word = 16'h0100;       // schema
                        7'd1: body_word = 16'h0001;       // power-on reset
                        7'd2: body_word = 16'h0000;       // old build id hi
                        7'd3: body_word = 16'h0000;       // old build id lo
                        7'd4: body_word = BUILD_ID[31:16];
                        7'd5: body_word = BUILD_ID[15:0];
                        default: body_word = 16'h0000;
                    endcase
                end
                K_HELLO: begin
                    case (pidx)
                        7'd0: body_word = 16'h584F;
                        7'd1: body_word = 16'h0100;
                        7'd2: body_word = BUILD_ID[31:16];
                        7'd3: body_word = BUILD_ID[15:0];
                        7'd4: body_word = 16'h0000;
                        7'd5: body_word = CAPABILITIES;
                        7'd6: body_word = 16'h0004;       // max payload
                        7'd7: body_word = 16'h0001;       // max outstanding
                        7'd8: body_word = 16'h0020;       // RX queue words
                        7'd9: body_word = 16'h0020;       // TX queue words
                        7'd10: body_word = 16'h0001;      // reset reason
                        default: body_word = 16'h0000;
                    endcase
                end
                K_INFO: begin
                    case (pidx)
                        7'd0: body_word = 16'h0100;       // info schema
                        7'd1: body_word = 16'h584F;       // device type
                        7'd2: body_word = 16'h0100;       // protocol version
                        7'd3: body_word = 16'h0101;       // slave + FMC16
                        7'd4: body_word = BUILD_ID[31:16];
                        7'd5: body_word = BUILD_ID[15:0];
                        7'd6: body_word = 16'h0100;       // schema is known
                        7'd7: body_word = 16'h0100;
                        7'd8: body_word = 16'h0100;
                        7'd9: body_word = 16'h0000;       // board revision unknown
                        7'd10: body_word = 16'h0801;      // selectable M0 + M11
                        7'd11: body_word = 16'h0000;
                        7'd12: body_word = 16'h0000;      // no experiment classes
                        7'd13: body_word = 16'h0000;
                        7'd14: body_word = 16'h0010;      // command only
                        default: body_word = 16'h0000;
                    endcase
                end
                K_ACK: begin
                    case (pidx)
                        7'd0: body_word = 16'h0030;       // command type
                        7'd1: body_word = 16'h0001;       // ACCEPTED
                        7'd2: body_word = build_opcode;    // accepted opcode
                        default: body_word = 16'h0000;
                    endcase
                end
                K_RESP: begin
                    if (build_opcode == 16'h0005) begin
                        case (pidx)
                            7'd0:  body_word = build_opcode;
                            7'd1:  body_word = build_status;
                            7'd2:  body_word = 16'h0000;
                            7'd3:  body_word = 16'd13;
                            7'd4:  body_word = (matrix_observer_present ? 16'h8000 : 16'h0000) |
                                                   ((matrix_snapshot_valid_columns == 16'hFFFF) ? 16'h4000 : 16'h0000) |
                                                   (build_detail[0] ? 16'h0100 : 16'h0000) |
                                                   {12'h000, platform_mode};
                            7'd5:  body_word = matrix_snapshot_valid_columns;
                            7'd6:  body_word = matrix_frame_sequence[31:16];
                            7'd7:  body_word = matrix_frame_sequence[15:0];
                            7'd8:  body_word = build_detail[0] ? matrix_snapshot[8]  : matrix_snapshot[0];
                            7'd9:  body_word = build_detail[0] ? matrix_snapshot[9]  : matrix_snapshot[1];
                            7'd10: body_word = build_detail[0] ? matrix_snapshot[10] : matrix_snapshot[2];
                            7'd11: body_word = build_detail[0] ? matrix_snapshot[11] : matrix_snapshot[3];
                            7'd12: body_word = build_detail[0] ? matrix_snapshot[12] : matrix_snapshot[4];
                            7'd13: body_word = build_detail[0] ? matrix_snapshot[13] : matrix_snapshot[5];
                            7'd14: body_word = build_detail[0] ? matrix_snapshot[14] : matrix_snapshot[6];
                            7'd15: body_word = build_detail[0] ? matrix_snapshot[15] : matrix_snapshot[7];
                            7'd16: body_word = matrix_capture_timestamp_ms[15:0];
                            default: body_word = 16'h0000;
                        endcase
                    end else if (build_opcode == 16'h0003) begin
                        case (pidx)
                            7'd0:  body_word = build_opcode;
                            7'd1:  body_word = build_status;
                            7'd2:  body_word = build_detail;
                            7'd3:  body_word = 16'd10;
                            7'd4:  body_word = 16'h0100;
                            7'd5:  body_word = 16'h0001 |
                                                   (po_valid  ? 16'h0002 : 16'h0000) |
                                                   (pio_valid ? 16'h0004 : 16'h0000) |
                                                   (fmc_clk_keep ? 16'h0100 : 16'h0000) |
                                                   (fmc_nwait_keep ? 16'h0200 : 16'h0000);
                            // Every profile uses the same latched-toggle PI
                            // convention, matching the proven M0 dataplane.
                            7'd6:  body_word = pi_requested;
                            7'd7:  body_word = {6'h00, ~key_stable_n};
                            7'd8:  body_word = {platform_mode, key_event_count[11:0]};
                            7'd9:  body_word = po_coherent[31:16];
                            7'd10: body_word = po_coherent[15:0];
                            7'd11: body_word = pio_coherent;
                            7'd12: body_word = timestamp_ms[31:16];
                            7'd13: body_word = timestamp_ms[15:0];
                            default: body_word = 16'h0000;
                        endcase
                    end else if (build_opcode == 16'h0004) begin
                        case (pidx)
                            7'd0: body_word = build_opcode;
                            7'd1: body_word = build_status;
                            7'd2: body_word = build_detail;
                            7'd3: body_word = 16'd1;
                            7'd4: body_word = {12'h000, platform_mode};
                            default: body_word = 16'h0000;
                        endcase
                    end else begin
                        case (pidx)
                            7'd0: body_word = build_opcode;
                            7'd1: body_word = build_status;
                            7'd2: body_word = build_detail;
                            7'd3: body_word = 16'd6;
                            7'd4: body_word = 16'h0000;
                            7'd5: body_word = CAPABILITIES;
                            7'd6: body_word = 16'h0004;
                            7'd7: body_word = 16'h0001;
                            7'd8: body_word = 16'h0020;
                            7'd9: body_word = 16'h0020;
                            default: body_word = 16'h0000;
                        endcase
                    end
                end
                K_NACK: begin
                    case (pidx)
                        7'd0: body_word = build_opcode;
                        7'd1: body_word = build_status;
                        7'd2: body_word = build_detail;
                        default: body_word = 16'h0000;
                    endcase
                end
                default: body_word = 16'h0000;
            endcase
        end
    endfunction

    function [15:0] build_word_for_idx;
        input [6:0] idx;
        reg [6:0] pidx;
        begin
            if (idx == 7'd0) build_word_for_idx = 16'hA55A;
            else if (idx == 7'd1) build_word_for_idx = 16'h5AA5;
            else if (idx == 7'd2) build_word_for_idx = 16'h100D;
            else if (idx == 7'd3) build_word_for_idx = build_total;
            else if (idx == 7'd4) build_word_for_idx = {type_for_kind(build_kind), 8'h00};
            else if (idx == 7'd5) build_word_for_idx = 16'h0100;
            else if (idx == 7'd6)
                build_word_for_idx = ((build_kind == K_RESP) || (build_kind == K_NACK)) ? 16'h0002 : 16'h0000;
            else if (idx == 7'd7) build_word_for_idx = {11'h000, payload_words_for_kind(build_kind)};
            else if (idx == 7'd8) build_word_for_idx = build_seq[31:16];
            else if (idx == 7'd9) build_word_for_idx = build_seq[15:0];
            else if (idx == 7'd10) build_word_for_idx = build_tid[31:16];
            else if (idx == 7'd11) build_word_for_idx = build_tid[15:0];
            else if (idx == 7'd12) build_word_for_idx = build_crc16;
            else if (idx == build_total - 7'd2) build_word_for_idx = build_crc32[31:16];
            else if (idx == build_total - 7'd1) build_word_for_idx = build_crc32[15:0];
            else begin
                pidx = idx - 7'd13;
                build_word_for_idx = body_word(build_kind, pidx);
            end
        end
    endfunction

    wire [15:0] build_word_decode = build_word_for_idx(build_idx);
    wire [15:0] build_crc16_next = crc16_next(build_word_q, build_crc16);
    wire [31:0] build_crc32_next = crc32_next(build_word_q, build_crc32);

    assign tx_word = tx_valid ? tx_mem[tx_ptr] : 16'hFFFF;
    assign tx_have_word = tx_valid;

    // ------------------------ RX parser ------------------------
    reg [5:0]  rx_idx;
    reg [15:0] rx_crc16;
    reg [31:0] rx_crc32;
    reg        rx_bad;
    reg [15:0] rx_type_flags;
    reg [15:0] rx_source_dest;
    reg [15:0] rx_channel;
    reg [15:0] rx_payload_words;
    reg [31:0] rx_tid;
    reg [15:0] rx_crc_hi;
    reg [15:0] rx_opcode;
    reg [15:0] rx_schema;
    reg [15:0] rx_options;
    reg [15:0] rx_timeout;

    // Start/queue a packet only when the serializer is idle.  The link test
    // has one outstanding command by contract, so no queue beyond these
    // explicit pending bits is needed.
    always @(posedge sys_clk or negedge rst_n) begin
        if (!rst_n) begin
            tx_count          <= 7'd0;
            tx_ptr            <= 7'd0;
            tx_valid          <= 1'b0;
            build_kind        <= K_RESET;
            build_total       <= 7'd23;
            build_idx         <= 7'd0;
            build_active      <= 1'b1;
            build_word_q      <= 16'h0000;
            build_word_valid  <= 1'b0;
            build_crc16       <= 16'hFFFF;
            build_crc32       <= 32'hFFFFFFFF;
            build_seq         <= 32'd0;
            build_tid         <= 32'd0;
            build_opcode      <= 16'd1;
            build_status      <= 16'd0;
            build_detail      <= 16'd0;
            tx_seq            <= 32'd0;
            pending_ack       <= 1'b0;
            pending_response  <= 1'b0;
            pending_nack      <= 1'b0;
            pending_tid       <= 32'd0;
            pending_opcode    <= 16'd0;
            pending_status    <= 16'd0;
            pending_detail    <= 16'd0;
            rx_idx            <= 6'd0;
            rx_crc16          <= 16'hFFFF;
            rx_crc32          <= 32'hFFFFFFFF;
            rx_bad            <= 1'b0;
            rx_type_flags     <= 16'd0;
            rx_source_dest    <= 16'd0;
            rx_channel        <= 16'd0;
            rx_payload_words  <= 16'd0;
            rx_tid            <= 32'd0;
            rx_crc_hi         <= 16'd0;
            rx_opcode         <= 16'd0;
            rx_schema         <= 16'd0;
            rx_options        <= 16'd0;
            rx_timeout        <= 16'd0;
            mode_command_value  <= 4'd0;
            mode_command_toggle <= 1'b0;
        end
        else begin
            if (build_active) begin
                if (!build_word_valid) begin
                    // Prime only.  The following cycle consumes this word,
                    // so build_word_decode never feeds the CRC registers in
                    // the same combinational path.
                    build_word_q     <= build_word_decode;
                    build_word_valid <= 1'b1;
                end
                else begin
                    tx_mem[build_idx] <= build_word_q;
                    if ((build_idx >= 7'd2) && (build_idx <= 7'd11)) begin
                        build_crc16 <= build_crc16_next;
                        build_crc32 <= build_crc32_next;
                    end
                    else if ((build_idx >= 7'd12) && (build_idx < build_total - 7'd2)) begin
                        build_crc32 <= build_crc32_next;
                    end

                    if (build_idx == build_total - 7'd1) begin
                        build_active     <= 1'b0;
                        build_word_valid <= 1'b0;
                        tx_valid         <= 1'b1;
                        tx_count         <= build_total;
                        tx_ptr           <= 7'd0;
                    end
                    else begin
                        // Wait one prime cycle before the next consume.  In
                        // particular, this lets idx=12 observe the newly
                        // registered header CRC16 from idx=11.
                        build_idx        <= build_idx + 7'd1;
                        build_word_valid <= 1'b0;
                    end
                end
            end
            else if (tx_valid && tx_advance) begin
                if (tx_ptr == tx_count - 7'd1) begin
                    tx_valid <= 1'b0;
                    tx_ptr   <= 7'd0;

                    // Boot sequence is exactly RESET_NOTICE -> LINK_HELLO
                    // -> DEVICE_INFO.  After that, service pending command.
                    if (build_kind == K_RESET) begin
                        build_kind   <= K_HELLO;
                        build_total  <= 7'd26;
                        build_seq    <= tx_seq + 32'd1;
                        tx_seq       <= tx_seq + 32'd1;
                        build_idx    <= 7'd0;
                        build_active <= 1'b1;
                        build_word_valid <= 1'b0;
                        build_crc16  <= 16'hFFFF;
                        build_crc32  <= 32'hFFFFFFFF;
                    end
                    else if (build_kind == K_HELLO) begin
                        build_kind   <= K_INFO;
                        build_total  <= 7'd31;
                        build_seq    <= tx_seq + 32'd1;
                        tx_seq       <= tx_seq + 32'd1;
                        build_idx    <= 7'd0;
                        build_active <= 1'b1;
                        build_word_valid <= 1'b0;
                        build_crc16  <= 16'hFFFF;
                        build_crc32  <= 32'hFFFFFFFF;
                    end
                    else if (build_kind == K_INFO) begin
                        // Count DEVICE_INFO as the third boot packet so the
                        // first command response starts at sequence 3.
                        tx_seq <= tx_seq + 32'd1;
                    end
                    else if (build_kind == K_ACK && pending_response) begin
                        build_kind   <= K_RESP;
                        build_total  <= (pending_opcode == 16'h0005) ? 7'd32 :
                                        (pending_opcode == 16'h0003) ? 7'd29 : 7'd25;
                        build_seq    <= tx_seq;
                        build_tid    <= pending_tid;
                        build_opcode <= pending_opcode;
                        build_status <= 16'h0000;
                        build_detail <= 16'h0000;
                        tx_seq       <= tx_seq + 32'd1;
                        pending_response <= 1'b0;
                        build_idx    <= 7'd0;
                        build_active <= 1'b1;
                        build_word_valid <= 1'b0;
                        build_crc16  <= 16'hFFFF;
                        build_crc32  <= 32'hFFFFFFFF;
                    end
                    else if (pending_ack || pending_response || pending_nack) begin
                        if (pending_ack) begin
                            build_kind   <= K_ACK;
                            build_total  <= 7'd19;
                            pending_ack  <= 1'b0;
                    build_opcode <= pending_opcode;
                        end
                        else if (pending_nack) begin
                            build_kind   <= K_NACK;
                            build_total  <= 7'd19;
                    build_opcode <= pending_opcode;
                            build_status <= pending_status;
                            build_detail <= pending_detail;
                            pending_nack <= 1'b0;
                        end
                        else begin
                            build_kind   <= K_RESP;
                            build_total  <= (pending_opcode == 16'h0005) ? 7'd32 :
                                            (pending_opcode == 16'h0003) ? 7'd29 : 7'd25;
                    build_opcode <= pending_opcode;
                            build_status <= pending_status;
                            build_detail <= pending_detail;
                            pending_response <= 1'b0;
                        end
                        build_tid    <= pending_tid;
                        build_seq    <= tx_seq;
                        tx_seq       <= tx_seq + 32'd1;
                        build_idx    <= 7'd0;
                        build_active <= 1'b1;
                        build_word_valid <= 1'b0;
                        build_crc16  <= 16'hFFFF;
                        build_crc32  <= 32'hFFFFFFFF;
                    end
                end
                else begin
                    tx_ptr <= tx_ptr + 7'd1;
                end
            end
            else if (!tx_valid && !build_active &&
                     (pending_ack || pending_response || pending_nack)) begin
                // No packet is currently on the bus (the normal state after
                // the three boot packets).  Start the first response directly;
                // the tx_advance path above chains ACK -> RESPONSE when needed.
                if (pending_ack) begin
                    build_kind  <= K_ACK;
                    build_total <= 7'd19;
                    pending_ack <= 1'b0;
                    build_opcode <= pending_opcode;
                end
                else if (pending_nack) begin
                    build_kind   <= K_NACK;
                    build_total  <= 7'd19;
                    build_opcode <= pending_opcode;
                    build_status <= pending_status;
                    build_detail <= pending_detail;
                    pending_nack <= 1'b0;
                end
                else begin
                    build_kind       <= K_RESP;
                    build_total      <= (pending_opcode == 16'h0005) ? 7'd32 :
                                        (pending_opcode == 16'h0003) ? 7'd29 : 7'd25;
                    build_opcode     <= pending_opcode;
                    build_status     <= pending_status;
                    build_detail     <= pending_detail;
                    pending_response <= 1'b0;
                end
                build_tid    <= pending_tid;
                build_seq    <= tx_seq;
                tx_seq       <= tx_seq + 32'd1;
                build_idx    <= 7'd0;
                build_active <= 1'b1;
                build_word_valid <= 1'b0;
                build_crc16  <= 16'hFFFF;
                build_crc32  <= 32'hFFFFFFFF;
            end

            if (rx_abort) begin
                rx_idx <= 6'd0;
                rx_bad <= 1'b0;
                rx_crc16 <= 16'hFFFF;
                rx_crc32 <= 32'hFFFFFFFF;
            end
            else if (rx_word_valid) begin
                case (rx_idx)
                    6'd0: begin
                        rx_idx <= (rx_word == 16'hA55A) ? 6'd1 : 6'd0;
                    end
                    6'd1: begin
                        if (rx_word == 16'h5AA5) rx_idx <= 6'd2;
                        else if (rx_word == 16'hA55A) rx_idx <= 6'd1;
                        else rx_idx <= 6'd0;
                    end
                    6'd2: begin
                        rx_bad   <= (rx_word != 16'h100D);
                        rx_crc16 <= crc16_next(rx_word, 16'hFFFF);
                        rx_crc32 <= crc32_next(rx_word, 32'hFFFFFFFF);
                        rx_idx   <= 6'd3;
                    end
                    6'd3: begin
                        rx_bad   <= rx_bad || (rx_word != 16'd19);
                        rx_crc16 <= crc16_next(rx_word, rx_crc16);
                        rx_crc32 <= crc32_next(rx_word, rx_crc32);
                        rx_idx   <= 6'd4;
                    end
                    6'd4: begin
                        rx_type_flags <= rx_word;
                        rx_crc16      <= crc16_next(rx_word, rx_crc16);
                        rx_crc32      <= crc32_next(rx_word, rx_crc32);
                        rx_idx         <= 6'd5;
                    end
                    6'd5: begin
                        rx_source_dest <= rx_word;
                        rx_crc16       <= crc16_next(rx_word, rx_crc16);
                        rx_crc32       <= crc32_next(rx_word, rx_crc32);
                        rx_idx          <= 6'd6;
                    end
                    6'd6: begin
                        rx_channel <= rx_word;
                        rx_crc16   <= crc16_next(rx_word, rx_crc16);
                        rx_crc32   <= crc32_next(rx_word, rx_crc32);
                        rx_idx      <= 6'd7;
                    end
                    6'd7: begin
                        rx_payload_words <= rx_word;
                        rx_bad <= rx_bad || (rx_word != 16'd4);
                        rx_crc16 <= crc16_next(rx_word, rx_crc16);
                        rx_crc32 <= crc32_next(rx_word, rx_crc32);
                        rx_idx <= 6'd8;
                    end
                    6'd8: begin
                        rx_crc16 <= crc16_next(rx_word, rx_crc16);
                        rx_crc32 <= crc32_next(rx_word, rx_crc32);
                        rx_idx   <= 6'd9;
                    end
                    6'd9: begin
                        rx_crc16 <= crc16_next(rx_word, rx_crc16);
                        rx_crc32 <= crc32_next(rx_word, rx_crc32);
                        rx_idx   <= 6'd10;
                    end
                    6'd10: begin
                        rx_tid[31:16] <= rx_word;
                        rx_crc16      <= crc16_next(rx_word, rx_crc16);
                        rx_crc32      <= crc32_next(rx_word, rx_crc32);
                        rx_idx        <= 6'd11;
                    end
                    6'd11: begin
                        rx_tid[15:0] <= rx_word;
                        rx_crc16 <= crc16_next(rx_word, rx_crc16);
                        rx_crc32 <= crc32_next(rx_word, rx_crc32);
                        rx_idx <= 6'd12;
                    end
                    6'd12: begin
                        rx_bad   <= rx_bad || (rx_word != rx_crc16);
                        rx_crc32 <= crc32_next(rx_word, rx_crc32);
                        rx_idx   <= 6'd13;
                    end
                    6'd13: begin
                        rx_opcode <= rx_word;
                        rx_crc32   <= crc32_next(rx_word, rx_crc32);
                        rx_idx     <= 6'd14;
                    end
                    6'd14: begin
                        rx_schema <= rx_word;
                        rx_crc32  <= crc32_next(rx_word, rx_crc32);
                        rx_idx    <= 6'd15;
                    end
                    6'd15: begin
                        rx_options <= rx_word;
                        rx_crc32   <= crc32_next(rx_word, rx_crc32);
                        rx_idx     <= 6'd16;
                    end
                    6'd16: begin
                        rx_timeout <= rx_word;
                        rx_crc32   <= crc32_next(rx_word, rx_crc32);
                        rx_idx     <= 6'd17;
                    end
                    6'd17: begin rx_crc_hi <= rx_word; rx_idx <= 6'd18; end
                    6'd18: begin
                        rx_idx <= 6'd0;
                        if (!rx_bad && ((rx_type_flags == 16'h3000) ||
                                        (rx_type_flags == 16'h3001)) &&
                            (rx_source_dest == 16'h0001) &&
                            (rx_channel == 16'h0002) &&
                            ((rx_opcode == 16'h0001) || (rx_opcode == 16'h0003) ||
                             (rx_opcode == 16'h0004) || (rx_opcode == 16'h0005)) &&
                            (rx_schema == 16'h0100) &&
                            (((rx_opcode == 16'h0004) &&
                              (rx_options[15:4] == 12'h000) &&
                              ((rx_options[3:0] == 4'd0) ||
                               (rx_options[3:0] == 4'd11))) ||
                             ((rx_opcode == 16'h0005) &&
                              (rx_options[15:1] == 15'h0000)) ||
                             (((rx_opcode != 16'h0004) &&
                               (rx_opcode != 16'h0005)) &&
                              (rx_options == 16'h0000))) &&
                            ({rx_crc_hi, rx_word} == rx_crc32)) begin
                            pending_tid      <= rx_tid;
                            pending_opcode   <= rx_opcode;
                            pending_status   <= 16'h0000;
                            pending_detail   <= (rx_opcode == 16'h0005) ?
                                                rx_options : 16'h0000;
                            pending_response <= 1'b1;
                            pending_ack      <= rx_type_flags[0];
                            if (rx_opcode == 16'h0004) begin
                                mode_command_value  <= rx_options[3:0];
                                mode_command_toggle <= ~mode_command_toggle;
                            end
                        end
                        else if (!rx_bad && ({rx_crc_hi, rx_word} == rx_crc32)) begin
                            pending_tid    <= rx_tid;
                            pending_opcode <= rx_opcode;
                            pending_status <= ((rx_opcode == 16'h0001) ||
                                               (rx_opcode == 16'h0003) ||
                                               (rx_opcode == 16'h0004) ||
                                               (rx_opcode == 16'h0005)) ? 16'h0009 : 16'h0002;
                            pending_detail <= 16'h0000;
                            pending_nack   <= 1'b1;
                        end
                    end
                    default: rx_idx <= 6'd0;
                endcase
            end
        end
    end

endmodule
