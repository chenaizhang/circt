//===- SystemCWrapVerilatedInstances.cpp - Select Verilator instances -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/SystemC/SystemCOps.h"
#include "circt/Dialect/SystemC/SystemCPasses.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"

namespace circt {
namespace systemc {
#define GEN_PASS_DEF_SYSTEMCWRAPVERILATEDINSTANCES
#include "circt/Dialect/SystemC/Passes.h.inc"
} // namespace systemc
} // namespace circt

using namespace mlir;
using namespace circt;

namespace {
struct SystemCWrapVerilatedInstancesPass
    : circt::systemc::impl::SystemCWrapVerilatedInstancesBase<
          SystemCWrapVerilatedInstancesPass> {
  using Base::Base;

  void runOnOperation() override {
    ModuleOp module = getOperation();
    SmallVector<hw::InstanceOp> instanceOps;
    module.walk(
        [&](hw::InstanceOp instance) { instanceOps.push_back(instance); });

    for (hw::InstanceOp instance : instanceOps) {
      Operation *target =
          SymbolTable::lookupNearestSymbolFrom(instance,
                                               instance.getModuleNameAttr());
      if (!target) {
        instance.emitError("cannot find referenced HW module ")
            << instance.getModuleName();
        signalPassFailure();
        return;
      }

      StringRef targetName = instance.getModuleName();
      bool selected = modules.empty() && instances.empty();
      if (!modules.empty())
        selected |= llvm::is_contained(modules, targetName.str());
      if (!instances.empty())
        selected |= llvm::is_contained(instances,
                                       instance.getInstanceName().str());

      // With no explicit selection, only wrap Verilator leaves. An explicit
      // --modules list may name either an extern leaf or an internal module.
      if (modules.empty() && instances.empty() &&
          !isa<hw::HWModuleExternOp>(target))
        continue;
      if (!selected)
        continue;

      auto targetModule = dyn_cast<hw::HWModuleLike>(target);
      if (!targetModule) {
        instance.emitError("referenced operation is not an HW module");
        signalPassFailure();
        return;
      }
      if (!instance.getParameters().empty()) {
        instance.emitError("parameterized Verilator instances are not supported");
        signalPassFailure();
        return;
      }

      OpBuilder builder(instance);
      SmallVector<Value> inputs(instance.getInputs());
      auto interop = systemc::InteropVerilatedOp::create(
          builder, instance.getLoc(), target, instance.getInstanceNameAttr(),
          inputs);
      instance.replaceAllUsesWith(interop.getResults());
      instance.erase();
    }
  }
};
} // namespace

std::unique_ptr<Pass>
circt::systemc::createSystemCWrapVerilatedInstancesPass() {
  return std::make_unique<SystemCWrapVerilatedInstancesPass>();
}
