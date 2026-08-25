//===-- Passes.td - SystemC pass definition file -----------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the passes that work on the SystemC dialect.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_SYSTEMC_PASSES_TD
#define CIRCT_DIALECT_SYSTEMC_PASSES_TD

include "mlir/Pass/PassBase.td"

def SystemCLowerInstanceInterop : Pass<"systemc-lower-instance-interop",
                                       "mlir::ModuleOp"> {
  let summary = "Lower all SystemC instance interop operations.";

  let constructor =  "circt::systemc::createSystemCLowerInstanceInteropPass()";
  let dependentDialects = [
    "systemc::SystemCDialect", "interop::InteropDialect",
    "mlir::emitc::EmitCDialect", "mlir::func::FuncDialect"
  ];
}

def SystemCWrapVerilatedInstances : Pass<"systemc-wrap-verilated-instances",
                                         "mlir::ModuleOp"> {
  let summary = "Replace selected hw.instance operations with Verilator interop instances.";
  let description = [{
    Replace `hw.instance` operations with `systemc.interop.verilated` operations
    before HW-to-SystemC conversion. If no module is selected, all instances of
    extern HW modules are wrapped. The resulting operations can then be lowered
    by `systemc-lower-instance-interop`.
  }];
  let constructor =
      "circt::systemc::createSystemCWrapVerilatedInstancesPass()";
  let options = [
    ListOption<"modules", "modules", "std::string",
               "HW module names to wrap; empty selects extern modules",
               "llvm::cl::ZeroOrMore,">
  ];
  let dependentDialects = [
    "hw::HWDialect", "systemc::SystemCDialect"
  ];
}

def SystemCContainerInteropLowering : Pass<"systemc-lower-container-interop",
                                           "mlir::ModuleOp"> {
  let summary = "Lower interop operations into their containing SystemC modules.";
  let constructor =
      "circt::systemc::createSystemCContainerInteropLoweringPass()";
  let dependentDialects = [
    "systemc::SystemCDialect", "interop::InteropDialect",
    "mlir::emitc::EmitCDialect", "mlir::func::FuncDialect"
  ];
}

#endif // CIRCT_DIALECT_SYSTEMC_PASSES_TD
