module soc_bw_read_window (
    input wire clk,
    input wire resetn,
    input wire clear,
    input wire arvalid,
    input wire arready,
    input wire rvalid,
    input wire rready,
    output wire [31:0] cycles_lo,
    output wire [31:0] cycles_hi
);
    reg active = 0;
    reg [63:0] elapsed = 0;
    reg [63:0] span = 0;
    wire address_accepted = arvalid && arready;
    wire data_accepted = rvalid && rready;

    assign cycles_lo = span[31:0];
    assign cycles_hi = span[63:32];

    always @(posedge clk) begin
        if (!resetn || clear) begin
            active <= 0;
            elapsed <= 0;
            span <= 0;
        end else if (active || address_accepted) begin
            active <= 1;
            elapsed <= elapsed + 1;
            // The last data handshake leaves the completed read window latched.
            if (data_accepted)
                span <= elapsed + 1;
        end
    end
endmodule
