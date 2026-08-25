// RUN: circt-opt --convert-hw-to-systemc %s \
// RUN:   | circt-translate --export-systemc \
// RUN:   | FileCheck %s

// Wide packed glue uses sc_bv rather than the sc_biguint 512-bit limit.  All
// three operations below must remain inlineable after HW conversion.
hw.module @wide_glue (in %a: i384, in %b: i384, in %sel: i1,
                      out y: i48) {
  %packed = comb.concat %a, %b : i384, i384
  %chosen = comb.mux %sel, %packed, %packed : i768
  %slice = comb.extract %chosen from 0 : (i768) -> i48
  hw.output %slice : i48
}

// CHECK: SC_MODULE(wide_glue)
// CHECK: sc_bv<768>
// CHECK: sc_bv<768>
// CHECK: .range(47, 0)
