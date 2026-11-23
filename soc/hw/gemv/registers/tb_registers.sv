`timescale 1ns/1ps
// Instantiate the ACTUAL HLS-generated AXI-Lite bank, not a replacement model.
// The compute-side signals below are synthetic: this test submits no AXI DMA.
module tb_gemv_registers;
    reg ACLK=0;
    always #5 ACLK=~ACLK;
    reg ARESET=1, ACLK_EN=1;
    reg [7:0] AWADDR=0, ARADDR=0;
    reg AWVALID=0, WVALID=0, BREADY=0, ARVALID=0, RREADY=0;
    reg [31:0] WDATA=0;
    reg [3:0] WSTRB=0;
    wire AWREADY,WREADY,BVALID,ARREADY,RVALID;
    wire [1:0] BRESP,RRESP;
    wire [31:0] RDATA;
    wire interrupt,ap_start;
    reg ap_done=0,ap_ready=0,ap_idle=1;
    reg [31:0] status=0;
    reg status_ap_vld=0;
    reg [63:0] read_bytes=0,write_bytes=0,macs=0;
    reg read_bytes_ap_vld=0,write_bytes_ap_vld=0,macs_ap_vld=0;
    wire [63:0] w_quants,w_scales,x_native,y;
    wire [31:0] rows,nb;
    integer reads=0,writes=0,checks=0;
    reg [6:0] offsets[0:9];
    reg [31:0] expected[0:9];

    soc_gemv_hw_control_s_axi dut (
        .ACLK(ACLK),.ARESET(ARESET),.ACLK_EN(ACLK_EN),
        .AWADDR(AWADDR),.AWVALID(AWVALID),.AWREADY(AWREADY),
        .WDATA(WDATA),.WSTRB(WSTRB),.WVALID(WVALID),.WREADY(WREADY),
        .BRESP(BRESP),.BVALID(BVALID),.BREADY(BREADY),
        .ARADDR(ARADDR),.ARVALID(ARVALID),.ARREADY(ARREADY),
        .RDATA(RDATA),.RRESP(RRESP),.RVALID(RVALID),.RREADY(RREADY),
        .interrupt(interrupt),.w_quants(w_quants),.w_scales(w_scales),
        .x_native(x_native),.y(y),.rows(rows),.nb(nb),
        .status(status),.status_ap_vld(status_ap_vld),
        .read_bytes(read_bytes),.read_bytes_ap_vld(read_bytes_ap_vld),
        .write_bytes(write_bytes),.write_bytes_ap_vld(write_bytes_ap_vld),
        .macs(macs),.macs_ap_vld(macs_ap_vld),
        .ap_start(ap_start),.ap_done(ap_done),.ap_ready(ap_ready),.ap_idle(ap_idle)
    );

    task automatic check(input bit condition,input string description);
        begin
            checks=checks+1;
            if (!condition) $fatal(1,"GEMV REGISTER RTL: FAIL: %s at %0t",description,$time);
        end
    endtask

    // AW and W are driven independently and held until their own handshake.
    // This supports HLS's address-before-data slave even when W arrives first.
    task automatic axi_write(input [7:0] addr,input [31:0] data,input [3:0] strobe,
                             input integer aw_delay,input integer w_delay,input integer stall);
        reg [1:0] held;
        begin
            @(negedge ACLK);
            AWVALID=0; WVALID=0; BREADY=0;
            fork
                begin
                    repeat(aw_delay) @(negedge ACLK);
                    AWADDR=addr; AWVALID=1;
                    do @(posedge ACLK); while (!AWREADY);
                    @(negedge ACLK); AWVALID=0;
                end
                begin
                    repeat(w_delay) @(negedge ACLK);
                    WDATA=data; WSTRB=strobe; WVALID=1;
                    do @(posedge ACLK); while (!WREADY);
                    @(negedge ACLK); WVALID=0;
                end
            join
            while (!BVALID) @(negedge ACLK);
            held=BRESP;
            check(held===2'b00,"write response OKAY");
            repeat(stall) begin
                @(posedge ACLK); #1;
                check(BVALID===1'b1 && BRESP===held,"B response held under backpressure");
            end
            @(negedge ACLK); BREADY=1;
            @(posedge ACLK);
            check(BVALID===1'b1,"B handshake");
            @(negedge ACLK); BREADY=0;
            writes=writes+1;
        end
    endtask

    task automatic axi_read(input [7:0] addr,input integer stall,output [31:0] data);
        reg [31:0] held;
        reg [1:0] response;
        begin
            @(negedge ACLK); ARADDR=addr; ARVALID=1; RREADY=0;
            do @(posedge ACLK); while (!ARREADY);
            @(negedge ACLK); ARVALID=0;
            while (!RVALID) @(negedge ACLK);
            held=RDATA; response=RRESP;
            check(response===2'b00,"read response OKAY");
            repeat(stall) begin
                @(posedge ACLK); #1;
                check(RVALID===1'b1 && RDATA===held && RRESP===response,
                      "R response held under backpressure");
            end
            @(negedge ACLK); RREADY=1;
            @(posedge ACLK); data=RDATA;
            check(RVALID===1'b1,"R handshake");
            @(negedge ACLK); RREADY=0;
            reads=reads+1;
        end
    endtask

    task automatic expect_read(input [7:0] addr,input [31:0] wanted,input [31:0] mask);
        reg [31:0] value;
        begin
            axi_read(addr,2,value);
            if ((value & mask)!==(wanted & mask))
                $fatal(1,"GEMV REGISTER RTL: FAIL: addr=%02x got=%08x expected=%08x mask=%08x",
                       addr,value,wanted,mask);
            checks=checks+1;
        end
    endtask

    task automatic check_parameters;
        integer i;
        reg [31:0] value;
        begin
            for(i=0;i<10;i=i+1) begin
                axi_read(offsets[i],i%3,value);
                check(value===expected[i],$sformatf("parameter readback/alias word %0d",i));
            end
            check(w_quants==={expected[1],expected[0]},"weight address to engine");
            check(w_scales==={expected[3],expected[2]},"scale address to engine");
            check(x_native==={expected[5],expected[4]},"input address to engine");
            check(y==={expected[7],expected[6]},"output address to engine");
            check(rows===expected[8] && nb===expected[9],"dimensions to engine");
            check(ap_start===1'b0,"parameter writes never start engine");
        end
    endtask

    task automatic reset_bank;
        integer i;
        begin
            @(negedge ACLK);
            AWVALID=0; WVALID=0; BREADY=0; ARVALID=0; RREADY=0;
            ap_done=0; ap_ready=0; ap_idle=1; status_ap_vld=0; status=0;
            read_bytes=0; write_bytes=0; macs=0;
            read_bytes_ap_vld=0; write_bytes_ap_vld=0; macs_ap_vld=0;
            ARESET=1;
            repeat(4) @(posedge ACLK);
            @(negedge ACLK); ARESET=0;
            repeat(3) @(posedge ACLK);
            for(i=0;i<10;i=i+1) expected[i]=0;
            check_parameters();
            expect_read(7'h00,32'h4,32'h87);
            expect_read(7'h04,0,32'hffffffff);
            expect_read(7'h08,0,32'hffffffff);
            expect_read(7'h0c,0,32'hffffffff);
            expect_read(7'h60,0,32'hffffffff);
            expect_read(7'h64,0,32'hffffffff);
            check(interrupt===1'b0,"interrupt reset");
        end
    endtask

    task automatic ready_pulse;
        begin
            @(negedge ACLK); ap_ready=1;
            @(posedge ACLK);
            @(negedge ACLK); ap_ready=0;
        end
    endtask

    integer i,p,b;
    reg [31:0] pattern,mask;
    initial begin
        // Any protocol hang, missing response or run-all failure must fail the run.
        #10000000;
        $fatal(1,"GEMV REGISTER RTL: FAIL: watchdog timeout");
    end
    initial begin
        offsets[0]=7'h10; offsets[1]=7'h14;
        offsets[2]=7'h20; offsets[3]=7'h24;
        offsets[4]=7'h30; offsets[5]=7'h34;
        offsets[6]=7'h40; offsets[7]=7'h44;
        offsets[8]=7'h50; offsets[9]=7'h58;
        reset_bank();

        $display("Register RTL: parameter patterns, address separation, AW/W skew and stalls");
        for(i=0;i<10;i=i+1) begin
            for(p=0;p<68;p=p+1) begin
                case(p)
                    0: pattern=0;
                    1: pattern=32'hffffffff;
                    2: pattern=32'haa55aa55;
                    3: pattern=32'h55aa55aa;
                    default: if(p<36) pattern=32'h1<<(p-4); else pattern=~(32'h1<<(p-36));
                endcase
                // Includes simultaneous, AW-before-W and W-before-AW arrivals.
                axi_write(offsets[i],pattern,4'hf,p%3==2 ? 2 : 0,p%3==1 ? 2 : 0,p%4);
                expected[i]=pattern;
                check_parameters();
            end
            axi_write(offsets[i],0,4'hf,0,0,0); expected[i]=0;
        end

        $display("Register RTL: byte strobes, zero strobe and reserved addresses");
        for(i=0;i<10;i=i+1) begin
            axi_write(offsets[i],32'h11223344,4'hf,0,2,1);
            expected[i]=32'h11223344;
            axi_write(offsets[i],32'hffffffff,4'h0,2,0,2);
            check_parameters();
            for(b=0;b<4;b=b+1) begin
                axi_write(offsets[i],32'ha5a5a5a5,4'h1<<b,1,0,2);
                mask=32'hff<<(b*8);
                expected[i]=(expected[i]&~mask)|(32'ha5a5a5a5&mask);
                check_parameters();
            end
        end
        axi_write(7'h54,32'hffffffff,4'hf,0,1,2);
        expect_read(7'h54,0,32'hffffffff);
        check_parameters();
        reset_bank();

        $display("Register RTL: start/ready handshake, done/status clear-on-read and RO status");
        axi_write(7'h00,1,4'h1,2,0,3);
        check(ap_start===1'b1,"START asserted");
        repeat(5) begin @(posedge ACLK); #1; check(ap_start===1'b1,"START held until READY"); end
        @(negedge ACLK); ap_idle=0;
        expect_read(7'h00,1,32'h85);
        ready_pulse();
        check(ap_start===1'b0,"START cleared on READY handshake");
        expect_read(7'h00,8,8);
        expect_read(7'h00,0,8);
        @(negedge ACLK); ap_done=1; ap_idle=1; status=3; status_ap_vld=1;
        read_bytes=64'h1122334455667788; write_bytes=64'h0102030405060708;
        macs=64'h8877665544332211;
        read_bytes_ap_vld=1; write_bytes_ap_vld=1; macs_ap_vld=1;
        @(posedge ACLK);
        @(negedge ACLK); ap_done=0; status_ap_vld=0;
        read_bytes_ap_vld=0; write_bytes_ap_vld=0; macs_ap_vld=0;
        axi_write(7'h00,0,4'hf,0,0,1);
        expect_read(7'h00,6,6);
        expect_read(7'h00,4,6);
        axi_write(7'h60,32'hdeadbeef,4'hf,0,1,0);
        expect_read(7'h60,3,32'hffffffff);
        axi_write(7'h64,0,4'hf,1,0,0);
        expect_read(7'h64,1,1);
        expect_read(7'h64,0,1);
        expect_read(8'h70,32'h55667788,32'hffffffff);
        expect_read(8'h74,32'h11223344,32'hffffffff);
        expect_read(8'h78,1,1);
        expect_read(8'h78,0,1);
        expect_read(8'h80,32'h05060708,32'hffffffff);
        expect_read(8'h84,32'h01020304,32'hffffffff);
        expect_read(8'h88,1,1);
        expect_read(8'h90,32'h44332211,32'hffffffff);
        expect_read(8'h94,32'h88776655,32'hffffffff);
        expect_read(8'h98,1,1);
        axi_write(7'h64,1,4'hf,0,0,0);
        expect_read(7'h64,0,1);
        @(negedge ACLK); ap_idle=0;
        axi_write(7'h00,4,4'hf,0,0,0);
        expect_read(7'h00,0,4);
        @(negedge ACLK); ap_idle=1;
        expect_read(7'h00,4,4);
        reset_bank();

        $display("Register RTL: auto-restart control and interrupt TOW registers");
        axi_write(7'h00,32'h81,4'h1,0,1,0);
        ready_pulse();
        check(ap_start===1'b1,"auto-restart retains START across handshake");
        expect_read(7'h00,32'h81,32'h81);
        axi_write(7'h00,0,4'h1,1,0,0);
        ready_pulse();
        check(ap_start===1'b0,"disabled auto-restart clears START at handshake");
        expect_read(7'h00,0,32'h81);
        axi_write(7'h04,32'hffffffff,4'hf,0,0,0);
        expect_read(7'h04,1,32'hffffffff);
        axi_write(7'h04,0,4'h0,0,0,0);
        expect_read(7'h04,1,32'hffffffff);
        axi_write(7'h08,32'hffffffff,4'hf,0,0,0);
        expect_read(7'h08,3,32'hffffffff);
        @(negedge ACLK); ap_done=1;
        @(posedge ACLK);
        @(negedge ACLK); ap_done=0;
        expect_read(7'h0c,1,3);
        expect_read(7'h0c,1,3);
        check(interrupt===1'b1,"done interrupt asserted");
        axi_write(7'h0c,1,4'h1,0,0,2);
        expect_read(7'h0c,0,3);
        check(interrupt===1'b0,"done interrupt cleared by toggle-on-write");
        ready_pulse();
        expect_read(7'h0c,2,3);
        axi_write(7'h0c,2,4'h1,0,0,0);
        expect_read(7'h0c,0,3);
        axi_write(7'h04,0,4'h1,0,0,0);
        check(interrupt===1'b0,"global interrupt disable");
        reset_bank();
        $display("GEMV REGISTER RTL: PASS; reads=%0d writes=%0d checks=%0d",reads,writes,checks);
        $finish;
    end
endmodule
