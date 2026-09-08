// RUN: circt-translate %s --export-systemc | FileCheck %s

// SystemC stores native child modules by value, which requires the complete
// child type to be emitted before the parent even when the IR lists the parent
// first.

// CHECK: SC_MODULE(Leaf)
// CHECK: SC_MODULE(Top)
systemc.module @Top() {
  %leaf = systemc.instance.decl @Leaf
    : !systemc.module<Leaf()>
}

systemc.module @Leaf() {
}
