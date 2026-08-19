// RUN: circt-translate %s --export-systemc | FileCheck %s

emitc.include <"systemc.h">

// CHECK-LABEL: SC_MODULE(comb_emission) {
systemc.module @comb_emission (
    %lhs: !systemc.in<!systemc.uint<17>>,
    %rhs: !systemc.in<!systemc.uint<17>>,
    %select: !systemc.in<i1>,
    %result: !systemc.out<!systemc.uint<17>>) {
  systemc.ctor {
    systemc.method %innerLogic
    systemc.sensitive %lhs, %rhs, %select : !systemc.in<!systemc.uint<17>>, !systemc.in<!systemc.uint<17>>, !systemc.in<i1>
  }
  %innerLogic = systemc.func {
    %lhs.read = systemc.signal.read %lhs : !systemc.in<!systemc.uint<17>>
    %lhs.int = systemc.convert %lhs.read : (!systemc.uint<17>) -> i17
    %rhs.read = systemc.signal.read %rhs : !systemc.in<!systemc.uint<17>>
    %rhs.int = systemc.convert %rhs.read : (!systemc.uint<17>) -> i17
    %select.read = systemc.signal.read %select : !systemc.in<i1>
    %sum = comb.add %lhs.int, %rhs.int : i17
    %xor = comb.xor %lhs.int, %rhs.int : i17
    %minimum = comb.icmp ult %lhs.int, %rhs.int : i17
    %selected = comb.mux %select.read, %sum, %xor : i17
    %out = systemc.convert %selected : (i17) -> !systemc.uint<17>
    systemc.signal.write %result, %out : !systemc.out<!systemc.uint<17>>
  }
}

// CHECK: void innerLogic() {
// CHECK: result.write(
// CHECK-SAME: select.read() ?
// CHECK-SAME: lhs.read()
// CHECK-SAME: rhs.read()
// CHECK-NOT: UNSUPPORTED OPERATION

// CHECK-LABEL: SC_MODULE(width_ops) {
systemc.module @width_ops(
    %input: !systemc.in<!systemc.uint<8>>,
    %parity: !systemc.out<i1>,
    %replicated: !systemc.out<!systemc.uint<16>>) {
  systemc.ctor {
    systemc.method %innerLogic
    systemc.sensitive %input : !systemc.in<!systemc.uint<8>>
  }
  %innerLogic = systemc.func {
    %read = systemc.signal.read %input : !systemc.in<!systemc.uint<8>>
    %value = systemc.convert %read : (!systemc.uint<8>) -> i8
    %parity_value = comb.parity %value : i8
    %replicated_value = comb.replicate %value : (i8) -> i16
    %replicated_out = systemc.convert %replicated_value : (i16) -> !systemc.uint<16>
    systemc.signal.write %parity, %parity_value : !systemc.out<i1>
    systemc.signal.write %replicated, %replicated_out : !systemc.out<!systemc.uint<16>>
  }
}

// CHECK: parity.write(
// CHECK-SAME: input.read()
// CHECK-SAME: .xor_reduce());
// CHECK: replicated.write(
// CHECK-SAME: sc_dt::concat(
// CHECK-NOT: UNSUPPORTED OPERATION

// CHECK-LABEL: SC_MODULE(complex_comb) {
systemc.module @complex_comb(
    %lhs: !systemc.in<!systemc.uint<16>>,
    %rhs: !systemc.in<!systemc.uint<16>>,
    %mixed: !systemc.out<!systemc.uint<16>>,
    %less: !systemc.out<i1>) {
  systemc.ctor {
    systemc.method %innerLogic
    systemc.sensitive %lhs, %rhs : !systemc.in<!systemc.uint<16>>, !systemc.in<!systemc.uint<16>>
  }
  %innerLogic = systemc.func {
    %lhs.read = systemc.signal.read %lhs : !systemc.in<!systemc.uint<16>>
    %lhs.int = systemc.convert %lhs.read : (!systemc.uint<16>) -> i16
    %rhs.read = systemc.signal.read %rhs : !systemc.in<!systemc.uint<16>>
    %rhs.int = systemc.convert %rhs.read : (!systemc.uint<16>) -> i16
    %high = comb.extract %lhs.int from 8 : (i16) -> i8
    %low = comb.extract %rhs.int from 0 : (i16) -> i8
    %joined = comb.concat %high, %low : i8, i8
    %quotient = comb.divs %lhs.int, %rhs.int : i16
    %remainder = comb.mods %lhs.int, %rhs.int : i16
    %shifted = comb.shrs %lhs.int, %rhs.int : i16
    %sum = comb.add %joined, %quotient, %remainder, %shifted : i16
    %less.value = comb.icmp slt %lhs.int, %rhs.int : i16
    %mixed.out = systemc.convert %sum : (i16) -> !systemc.uint<16>
    systemc.signal.write %mixed, %mixed.out : !systemc.out<!systemc.uint<16>>
    systemc.signal.write %less, %less.value : !systemc.out<i1>
  }
}

// CHECK: sc_dt::concat(
// CHECK: .range(15, 8)
// CHECK: .range(7, 0)
// CHECK: sc_int<16>(
// CHECK-SAME: /
// CHECK: sc_int<16>(
// CHECK-SAME: %
// CHECK: sc_int<16>(
// CHECK-SAME: >>
// CHECK: sc_int<16>(
// CHECK-SAME: <
// CHECK-NOT: UNSUPPORTED OPERATION
