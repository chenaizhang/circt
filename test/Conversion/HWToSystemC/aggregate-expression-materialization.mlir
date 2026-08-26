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
