//===- ExtractHierarchySlice.cpp - Depth-bounded HW hierarchy -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/HWPasses.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <deque>
#include <system_error>

namespace circt {
namespace hw {
#define GEN_PASS_DEF_EXTRACTHIERARCHYSLICE
#include "circt/Dialect/HW/Passes.h.inc"
} // namespace hw
} // namespace circt

using namespace circt;
using namespace circt::hw;
using namespace mlir;

namespace {

struct EdgeInfo {
  std::string parent;
  std::string instance;
  std::string target;
  unsigned parentDepth;
  bool retained;
};

static llvm::json::Array getPortContract(HWModuleLike module) {
  llvm::json::Array ports;
  for (const auto &port : module.getPortList()) {
    std::string type;
    llvm::raw_string_ostream os(type);
    port.type.print(os);
    StringRef direction = port.isInput()    ? "input"
                          : port.isOutput() ? "output"
                                            : "inout";
    ports.push_back(
        llvm::json::Object{{"name", port.name.getValue().str()},
                           {"direction", direction.str()},
                           {"type", os.str()}});
  }
  return ports;
}

struct ExtractHierarchySlicePass
    : public circt::hw::impl::ExtractHierarchySliceBase<
          ExtractHierarchySlicePass> {
  using Base::Base;

  void runOnOperation() override {
    ModuleOp design = getOperation();
    if (topName.empty()) {
      design.emitError("hw-extract-hierarchy-slice requires --top");
      return signalPassFailure();
    }

    llvm::StringMap<HWModuleLike> modules;
    for (Operation &op : *design.getBody())
      if (auto module = dyn_cast<HWModuleLike>(&op))
        modules[module.getModuleName()] = module;

    auto topIt = modules.find(topName);
    if (topIt == modules.end() || !isa<HWModuleOp>(topIt->second.getOperation())) {
      design.emitError("top module '") << topName
                                         << "' is not an hw.module";
      return signalPassFailure();
    }

    llvm::StringMap<unsigned> depth;
    std::deque<std::string> worklist;
    depth[topName] = 0;
    worklist.push_back(topName);
    SmallVector<EdgeInfo> edges;

    while (!worklist.empty()) {
      std::string parentName = std::move(worklist.front());
      worklist.pop_front();
      unsigned parentDepth = depth.lookup(parentName);
      auto parent = dyn_cast<HWModuleOp>(modules.lookup(parentName).getOperation());
      if (!parent || parentDepth >= maxDepth)
        continue;

      for (InstanceOp instance : parent.getBodyBlock()->getOps<InstanceOp>()) {
        std::string target = instance.getModuleName().str();
        edges.push_back({parentName, instance.getInstanceName().str(), target,
                         parentDepth, true});
        if (!modules.count(target)) {
          instance.emitError("hierarchy target '") << target
                                                     << "' was not found";
          return signalPassFailure();
        }
        unsigned childDepth = parentDepth + 1;
        auto [it, inserted] = depth.try_emplace(target, childDepth);
        if (inserted) {
          worklist.push_back(target);
        } else if (childDepth < it->second) {
          it->second = childDepth;
          worklist.push_back(target);
        }
      }
    }

    // Record the complete instance boundary before replacing frontier bodies.
    for (auto &entry : depth) {
      if (entry.getValue() != maxDepth)
        continue;
      auto module = dyn_cast<HWModuleOp>(
          modules.lookup(entry.getKey()).getOperation());
      if (!module)
        continue;
      for (InstanceOp instance : module.getBodyBlock()->getOps<InstanceOp>())
        edges.push_back({entry.getKey().str(), instance.getInstanceName().str(),
                         instance.getModuleName().str(), entry.getValue(),
                         false});
    }

    SmallVector<std::string> retained;
    SmallVector<std::string> frontier;
    SmallVector<std::string> removed;
    for (auto &entry : modules) {
      auto found = depth.find(entry.getKey());
      if (found == depth.end() || found->second > maxDepth) {
        removed.push_back(entry.getKey().str());
        continue;
      }
      retained.push_back(entry.getKey().str());
      if (found->second == maxDepth)
        frontier.push_back(entry.getKey().str());
    }
    llvm::sort(retained);
    llvm::sort(frontier);
    llvm::sort(removed);

    // Replace frontier definitions with external declarations.  This keeps a
    // verifier-clean HW design while making it impossible for later passes to
    // accidentally lower behavior below the selected boundary.
    for (StringRef name : frontier) {
      auto module = dyn_cast<HWModuleOp>(modules.lookup(name).getOperation());
      if (!module)
        continue;
      OpBuilder builder(module);
      SmallVector<NamedAttribute> attributes;
      attributes.emplace_back(builder.getStringAttr("hw.hierarchy.frontier"),
                              builder.getUnitAttr());
      attributes.emplace_back(builder.getStringAttr("hw.hierarchy.depth"),
                              builder.getI64IntegerAttr(depth.lookup(name)));
      auto ext = HWModuleExternOp::create(
          builder, module.getLoc(), module.getModuleNameAttr(),
          module.getPortList(), StringRef(), module.getParameters(), attributes);
      ext.setVisibility(module.getVisibility());
      module.erase();
      modules[name] = ext;
    }

    // Remove every definition outside the selected hierarchy. References from
    // frontier bodies disappeared when those bodies were externalized.
    for (StringRef name : removed)
      if (auto module = modules.lookup(name))
        module.erase();

    if (manifestFilename.empty())
      return;

    llvm::json::Array retainedJSON, frontierJSON, removedJSON, edgeJSON,
        moduleJSON;
    for (const auto &name : retained)
      retainedJSON.push_back(name);
    for (const auto &name : frontier)
      frontierJSON.push_back(name);
    for (const auto &name : removed)
      removedJSON.push_back(name);
    for (const auto &edge : edges)
      edgeJSON.push_back(llvm::json::Object{
          {"parent", edge.parent},
          {"instance", edge.instance},
          {"target", edge.target},
          {"parent_depth", static_cast<int64_t>(edge.parentDepth)},
          {"retained", edge.retained}});
    for (const auto &name : retained) {
      auto module = modules.lookup(name);
      moduleJSON.push_back(llvm::json::Object{
          {"name", name},
          {"depth", static_cast<int64_t>(depth.lookup(name))},
          {"frontier", depth.lookup(name) == maxDepth},
          {"ports", getPortContract(module)}});
    }

    llvm::json::Object manifest{
        {"schema", "circt.hw.hierarchy-slice.v1"},
        {"top", topName.getValue()},
        {"max_depth", static_cast<int64_t>(maxDepth.getValue())},
        {"retained_modules", std::move(retainedJSON)},
        {"frontier_modules", std::move(frontierJSON)},
        {"removed_modules", std::move(removedJSON)},
        {"modules", std::move(moduleJSON)},
        {"instances", std::move(edgeJSON)}};
    std::error_code error;
    llvm::raw_fd_ostream output(manifestFilename, error);
    if (error) {
      design.emitError("cannot write hierarchy manifest '")
          << manifestFilename << "': " << error.message();
      return signalPassFailure();
    }
    output << llvm::formatv("{0:2}\n",
                            llvm::json::Value(std::move(manifest)));
  }
};

} // namespace
