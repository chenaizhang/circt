// RUN: circt-translate --export-systemc %s | FileCheck %s

emitc.include <"systemc.h">
emitc.include <"array">

systemc.module @edge_detector(%clk: !systemc.in<i1>,
                              %edge: !systemc.out<i1>) {
  systemc.ctor {
    systemc.method %innerLogic
    systemc.sensitive %clk : !systemc.in<i1>
  }
  %innerLogic = systemc.func {
    %posedge = systemc.signal.posedge %clk : !systemc.in<i1>
    systemc.signal.write %edge, %posedge : !systemc.out<i1>
  }
}

// CHECK: SC_CTOR(edge_detector) {
// CHECK: SC_METHOD(innerLogic);
// CHECK: sensitive << clk;
// CHECK: void innerLogic() {
// CHECK: edge.write(clk.posedge());
// CHECK-NOT: UNSUPPORTED OPERATION

// CHECK-LABEL: SC_MODULE(memory)
// CHECK: std::array<sc_uint<8>, 16> storage{};
// CHECK: SC_METHOD(innerLogic);
// CHECK: if ((sc_uint<1>(clk.posedge() & writeEnable.read())) != 0)
// CHECK-SAME: storage[sc_uint<4>(address.read())] = sc_uint<8>(writeData.read());
// CHECK: readData.write(sc_uint<8>(storage[sc_uint<4>(address.read())]));
systemc.module @memory(%clk: !systemc.in<i1>,
                       %writeEnable: !systemc.in<i1>,
                       %address: !systemc.in<!systemc.uint<4>>,
                       %writeData: !systemc.in<!systemc.uint<8>>,
                       %readData: !systemc.out<!systemc.uint<8>>) {
  %storage = systemc.memory "storage" [16 x 8] latency 0, 1 : !emitc.opaque<"std::array<sc_uint<8>, 16>">
  systemc.ctor {
    systemc.method %innerLogic
    systemc.sensitive %clk, %writeEnable, %address, %writeData : !systemc.in<i1>, !systemc.in<i1>, !systemc.in<!systemc.uint<4>>, !systemc.in<!systemc.uint<8>>
  }
  %innerLogic = systemc.func {
    %edge = systemc.signal.posedge %clk : !systemc.in<i1>
    %enable = systemc.signal.read %writeEnable : !systemc.in<i1>
    %condition = comb.and %edge, %enable : i1
    %addressValue = systemc.signal.read %address : !systemc.in<!systemc.uint<4>>
    %dataValue = systemc.signal.read %writeData : !systemc.in<!systemc.uint<8>>
    %addressInt = systemc.convert %addressValue : (!systemc.uint<4>) -> i4
    %dataInt = systemc.convert %dataValue : (!systemc.uint<8>) -> i8
    systemc.memory.write %storage[%addressInt] = %dataInt if %condition : !emitc.opaque<"std::array<sc_uint<8>, 16>">, i4, i8, i1
    %value = systemc.memory.read %storage[%addressInt] : !emitc.opaque<"std::array<sc_uint<8>, 16>">, i4 -> i8
    %output = systemc.convert %value : (i8) -> !systemc.uint<8>
    systemc.signal.write %readData, %output : !systemc.out<!systemc.uint<8>>
  }
}
