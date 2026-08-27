module Leaf (
    input  logic [31:0] a,
    output logic [31:0] c
);
  assign c = a + 32'd1;
endmodule
