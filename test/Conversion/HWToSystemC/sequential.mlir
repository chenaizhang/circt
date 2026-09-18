// RUN: circt-opt --convert-hw-to-systemc %s | FileCheck %s
// RUN: circt-opt --convert-hw-to-systemc %s | circt-translate --export-systemc > /dev/null

hw.module @counter(in %clk: i1, in %reset: i1, in %enable: i1,
                   in %next: i8, out value: i8) {
  %clock = seq.to_clock %clk
  %zero = hw.constant 0 : i8
  %value = seq.compreg.ce name "value" %next, %clock, %enable
      reset %reset, %zero : i8
  hw.output %value : i8
}

// CHECK-LABEL: systemc.module @counter
// CHECK: %value_state = systemc.signal : !systemc.signal<!systemc.uint<8>>
// CHECK: systemc.sensitive {{.*}}%value_state
// CHECK: %[[STATE:.*]] = systemc.signal.read %value_state
// CHECK: "systemc.register.write"(%value_state, {{.*}}, %clk, {{.*}}, {{.*}}, {{.*}}) <{isAsync = false}>
// CHECK-NOT: seq.

// -----

// Wide memory data must be converted before reaching ExportSystemC. Keeping
// the read result as builtin i192 would leave the exporter without a C++ type.
hw.module @wide_memory(in %clk: i1, in %address: i5,
                       out readData: i192) {
  %clock = seq.to_clock %clk
  %storage = seq.firmem 0, 1, undefined, undefined : <32 x 192>
  %readData = seq.firmem.read_port %storage[%address], clock %clock
      : <32 x 192>
  hw.output %readData : i192
}

// CHECK-LABEL: systemc.module @wide_memory
// CHECK: systemc.memory.read {{.*}} -> !systemc.biguint<192>
// CHECK: systemc.convert {{.*}} : (!systemc.biguint<192>) -> i192
// CHECK-NOT: seq.

// -----

// A simulation memory maps to a fixed-size C++ array. The asynchronous read
// remains combinational and the write is guarded by the clock edge and enable.
hw.module @memory(in %clk: i1, in %writeEnable: i1, in %address: i4,
                  in %writeData: i8, out readData: i8) {
  %clock = seq.to_clock %clk
  %storage = seq.firmem 0, 1, undefined, undefined : <16 x 8>
  %readData = seq.firmem.read_port %storage[%address], clock %clock : <16 x 8>
  seq.firmem.write_port %storage[%address] = %writeData, clock %clock
      enable %writeEnable : <16 x 8>
  hw.output %readData : i8
}

// CHECK-LABEL: systemc.module @memory
// CHECK: systemc.memory {{.*}}[16 x 8] latency 0, 1
// CHECK: systemc.memory.read
// CHECK: systemc.signal.posedge %clk
// CHECK: comb.and
// CHECK: systemc.memory.write
// CHECK-NOT: seq.

// -----

hw.module @async_register(in %clk: i1, in %reset: i1, in %next: i8,
                          out value: i8) {
  %clock = seq.to_clock %clk
  %zero = hw.constant 0 : i8
  %value = seq.firreg %next clock %clock reset async %reset, %zero : i8
  hw.output %value : i8
}

// CHECK-LABEL: systemc.module @async_register
// CHECK: %[[STATE_SIGNAL:.*]] = systemc.signal : !systemc.signal<!systemc.uint<8>>
// CHECK: "systemc.register.write"(%[[STATE_SIGNAL]], {{.*}}, %clk, %true, {{.*}}, {{.*}}) <{isAsync = true}>
// CHECK-NOT: seq.

// -----

// A constant next-state producer may be legalized before its consumer. The
// register conversion must use the adapted value without recursively invoking
// the dialect converter's target materializer.
hw.module @constant_register(in %clk: i1, out value: i8) {
  %clock = seq.to_clock %clk
  %zero = hw.constant 0 : i8
  %value = seq.compreg %zero, %clock : i8
  hw.output %value : i8
}

// CHECK-LABEL: systemc.module @constant_register
// CHECK: "systemc.register.write"(%{{.*}}, %{{.*}}, %clk, %true, %false, %{{.*}}) <{isAsync = false}>
// CHECK-NOT: seq.

// -----

// Exercise a graph-region feedback edge where the register input is defined
// after the register. Lowering must replace the feedback with a current-state
// signal read and leave the add in the combinational next-state cone.
hw.module @feedback_counter(in %clk: i1, in %reset: i1, out value: i8) {
  %clock = seq.to_clock %clk
  %zero = hw.constant 0 : i8
  %one = hw.constant 1 : i8
  %value = seq.compreg name "value" %next, %clock reset %reset, %zero : i8
  %next = comb.add %value, %one : i8
  hw.output %value : i8
}

// CHECK-LABEL: systemc.module @feedback_counter
// CHECK: %[[STATE_SIGNAL:.*]] = systemc.signal : !systemc.signal<!systemc.uint<8>>
// CHECK: %[[STATE_READ:.*]] = systemc.signal.read %[[STATE_SIGNAL]]
// CHECK: %[[CURRENT:.*]] = systemc.convert %[[STATE_READ]] : (!systemc.uint<8>) -> i8
// CHECK: %[[NEXT:.*]] = comb.add {{.*}}, {{.*}} : i8
// CHECK: %[[NEXT_BV:.*]] = systemc.convert %[[NEXT]] : (i8) -> !systemc.bv<8>
// CHECK: %[[NEXT_VAR:.*]] = systemc.cpp.variable %[[NEXT_BV]] : !systemc.bv<8>
// CHECK: %[[NEXT_INT:.*]] = systemc.convert %[[NEXT_VAR]] : (!systemc.bv<8>) -> i8
// CHECK: %[[NEXT_SC:.*]] = systemc.convert %[[NEXT_INT]] : (i8) -> !systemc.uint<8>
// CHECK: "systemc.register.write"(%[[STATE_SIGNAL]], %[[NEXT_SC]], %clk, %true, {{.*}}, {{.*}}) <{isAsync = false}>
// CHECK-NOT: seq.
