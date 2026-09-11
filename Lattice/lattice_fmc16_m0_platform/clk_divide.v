module clk_divide (
    input  wire sys_clk,
    input  wire rst_n,
    output reg  clk_1k
);

    //50MHz--1kHz
    parameter DIVIDER = 25000;
    localparam [15:0] DIVIDER_COUNT = DIVIDER - 1;
    reg [15:0] counter;

    always @(posedge sys_clk or negedge rst_n) begin
        if (!rst_n) begin
            counter <= 0;
            clk_1k <= 0;
        end
        else begin
            if (counter == DIVIDER_COUNT) begin
                counter <= 0;
                clk_1k <= ~clk_1k;
            end
            else begin
                counter <= counter + 16'd1;
            end
        end
    end

endmodule
