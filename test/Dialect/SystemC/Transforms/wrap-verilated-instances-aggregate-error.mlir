// RUN: not circt-opt --systemc-wrap-verilated-instances="modules=Leaf" %s 2>&1 | FileCheck %s

!Cfg = !hw.struct<enable: i1, mode: i3>

hw.module @Leaf(in %cfg: !Cfg, out result: i3) {
  %mode = hw.struct_extract %cfg["mode"] : !Cfg
  hw.output %mode : i3
}

hw.module @Top(in %cfg: !Cfg, out result: i3) {
  // CHECK: error: Verilator interop requires a scalar port ABI
  %result = hw.instance "leaf" @Leaf(cfg: %cfg: !Cfg) -> (result: i3)
  hw.output %result : i3
}
