// RUN: circt-opt --hw-extract-hierarchy-slice="top=Top max-depth=1" %s | FileCheck %s --check-prefix=DEPTH1
// RUN: circt-opt --hw-extract-hierarchy-slice="top=Top max-depth=2" %s | FileCheck %s --check-prefix=DEPTH2
// RUN: circt-opt --hw-extract-hierarchy-slice="top=Top max-depth=1" --convert-hw-to-systemc="structure-only=true" %s | FileCheck %s --check-prefix=SYSTEMC
// RUN: circt-opt --hw-extract-hierarchy-slice="top=Top max-depth=1 manifest=%t.json" %s -o /dev/null
// RUN: FileCheck %s --check-prefix=MANIFEST < %t.json

// DEPTH1-LABEL: hw.module @Top
// DEPTH1: hw.instance "mid" @Mid
// DEPTH1-LABEL: hw.module.extern private @Mid
// DEPTH1-SAME: hw.hierarchy.frontier
// DEPTH1-NOT: hw.module private @Leaf
// DEPTH1-NOT: comb.add

// DEPTH2-LABEL: hw.module @Top
// DEPTH2: hw.instance "mid" @Mid
// DEPTH2-LABEL: hw.module private @Mid
// DEPTH2: hw.instance "leaf" @Leaf
// DEPTH2-LABEL: hw.module.extern private @Leaf
// DEPTH2-SAME: hw.hierarchy.frontier
// DEPTH2-NOT: comb.add

// SYSTEMC-LABEL: systemc.module private @Mid
// SYSTEMC-SAME: systemc.hierarchy.frontier
// SYSTEMC: %behaviorSlot = systemc.func
// SYSTEMC: systemc.method %behaviorSlot
// SYSTEMC-LABEL: systemc.module @Top
// SYSTEMC: systemc.instance.decl {{.*}} @Mid
// SYSTEMC-NOT: @Leaf

// MANIFEST: "schema": "circt.hw.hierarchy-slice.v1"
// MANIFEST: "top": "Top"
// MANIFEST: "max_depth": 1
// MANIFEST: "frontier_modules": [
// MANIFEST-NEXT: "Mid"
// MANIFEST: "parent": "Top"
// MANIFEST: "retained": true
// MANIFEST: "target": "Mid"
// MANIFEST: "parent": "Mid"
// MANIFEST: "retained": false
// MANIFEST: "target": "Leaf"

hw.module @Top(in %a: i8, out y: i8) {
  %y = hw.instance "mid" @Mid(a: %a: i8) -> (y: i8)
  hw.output %y : i8
}

hw.module private @Mid(in %a: i8, out y: i8) {
  %y = hw.instance "leaf" @Leaf(a: %a: i8) -> (y: i8)
  hw.output %y : i8
}

hw.module private @Leaf(in %a: i8, out y: i8) {
  %one = hw.constant 1 : i8
  %y = comb.add %a, %one : i8
  hw.output %y : i8
}
