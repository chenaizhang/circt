// RUN: circt-verilog --ir-hw %s | FileCheck %s

module TaskNoTiming(
  input  logic [4:0] lhs,
  input  logic [4:0] rhs,
  output logic [4:0] result
);
  task automatic select_min(
    input  logic [4:0] a,
    input  logic [4:0] b,
    output logic [4:0] selected
  );
    if (a < b)
      selected = a;
    else
      selected = b;
  endtask

  always_comb select_min(lhs, rhs, result);
endmodule

// CHECK-LABEL: hw.module @TaskNoTiming
// CHECK: comb.icmp
// CHECK: comb.mux
// CHECK-NOT: llhd.coroutine
// CHECK-NOT: llhd.call_coroutine

