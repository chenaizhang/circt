// RUN: circt-opt --llhd-lower-timed-processes %s | FileCheck %s

// CHECK-LABEL: hw.module @counter
hw.module @counter(in %clk : i1, out count : i2) {
  %c0_i2 = hw.constant 0 : i2
  %c1_i2 = hw.constant 1 : i2
  %true = hw.constant true
  %t = llhd.constant_time <1ns, 0d, 0e>
  // CHECK: %[[TO_CLOCK:.+]] = seq.to_clock %clk
  // CHECK: %[[REG:.+]] = seq.compreg %{{.+}}, %[[TO_CLOCK]] : i2
  // CHECK: hw.output %[[REG]] : i2
  %sig = llhd.sig %c0_i2 : i2
  %0:1 = llhd.process -> i2 {
    cf.br ^bb1(%c0_i2 : i2)
  ^bb1(%state: i2):
    llhd.wait yield (%state : i2), (%clk : i1), ^bb2(%clk : i1)
  ^bb2(%prev_clk: i1):
    %not_prev = comb.xor %prev_clk, %true : i1
    %edge = comb.and %not_prev, %clk : i1
    cf.cond_br %edge, ^bb3, ^bb1(%c0_i2 : i2)
  ^bb3:
    %inc = comb.add bin %state, %c1_i2 : i2
    cf.br ^bb1(%inc : i2)
  }
  llhd.drv %sig, %0 after %t : i2
  %1 = llhd.prb %sig : i2
  hw.output %1 : i2
}
