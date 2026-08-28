// RUN: circt-opt --convert-hw-to-systemc="prepared-input=true" %s | FileCheck %s

// Shared packed expressions must be bound once. ExportSystemC otherwise
// recursively duplicates their expression trees at every use.
// CHECK-LABEL: systemc.module @Top
// CHECK: systemc.cpp.variable
hw.module @Top(in %hi: i64, in %lo: i64, out y: i128) {
  %packed = comb.concat %hi, %lo : i64, i64
  %upper = comb.extract %packed from 64 : (i128) -> i64
  %lower = comb.extract %packed from 0 : (i128) -> i64
  %result = comb.concat %upper, %lower : i64, i64
  hw.output %result : i128
}

// A shared one-bit packed expression must remain a native boolean. Converting
// an sc_bv<1> temporary back to i1 would export as invalid `bool(sc_bv<1>)`.
// CHECK-LABEL: systemc.module @SharedBit
// CHECK: %[[BIT:.+]] = comb.extract
// CHECK-NEXT: %[[VAR:.+]] = systemc.cpp.variable %[[BIT]] : i1
// CHECK-NOT: !systemc.bv<1>
hw.module @SharedBit(in %packed: i8, out a: i1, out b: i1) {
  %bit = comb.extract %packed from 3 : (i8) -> i1
  hw.output %bit, %bit : i1, i1
}
