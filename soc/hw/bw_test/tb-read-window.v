`timescale 1ns/1ps
module tb_read_window;
    reg clk = 0;
    reg resetn = 0;
    reg clear = 0;
    reg arvalid = 0;
    reg arready = 0;
    reg rvalid = 0;
    reg rready = 0;
    wire [31:0] cycles_lo;
    wire [31:0] cycles_hi;
    integer failures = 0;

    soc_bw_read_window dut (
        .clk(clk), .resetn(resetn), .clear(clear),
        .arvalid(arvalid), .arready(arready), .rvalid(rvalid), .rready(rready),
        .cycles_lo(cycles_lo), .cycles_hi(cycles_hi)
    );
    always #5 clk = ~clk;

    task step;
        input av, ar, rv, rr, cl;
        begin
            @(negedge clk);
            arvalid = av; arready = ar; rvalid = rv; rready = rr; clear = cl;
            @(posedge clk);
            #1;
        end
    endtask

    task expect_cycles;
        input [63:0] expected;
        begin
            if ({cycles_hi, cycles_lo} !== expected) begin
                $display("FAIL span=%0d expected=%0d", {cycles_hi, cycles_lo}, expected);
                failures = failures + 1;
            end
        end
    endtask

    initial begin
        step(0, 0, 0, 0, 0);
        resetn = 1;
        step(1, 0, 0, 0, 0);
        step(0, 0, 0, 0, 0);
        expect_cycles(0);
        step(1, 1, 0, 0, 0); // Cycle 1: first address.
        step(0, 0, 1, 0, 0); // Data backpressure counts as time.
        step(0, 0, 1, 1, 0);
        expect_cycles(3);
        step(1, 1, 1, 1, 0); // Overlapping next burst.
        step(0, 0, 0, 1, 0);
        step(0, 0, 1, 1, 0);
        expect_cycles(6);
        repeat (4) step(0, 0, 0, 0, 0);
        expect_cycles(6); // Polling after completion cannot change the span.
        step(0, 0, 0, 0, 1);
        step(0, 0, 0, 0, 0);
        expect_cycles(0); // Empty job.
        step(1, 1, 1, 1, 0);
        expect_cycles(1);
        resetn = 0;
        step(0, 0, 0, 0, 0);
        expect_cycles(0);
        if (failures) $fatal(1, "read window: %0d failures", failures);
        $display("read window: PASS");
        $finish;
    end
endmodule
