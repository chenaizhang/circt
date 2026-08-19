//===- InlineSuspendFreeCoroutines.cpp ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/LLHD/LLHDPasses.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"

namespace circt {
namespace llhd {
#define GEN_PASS_DEF_INLINESUSPENDFREECOROUTINESPASS
#include "circt/Dialect/LLHD/LLHDPasses.h.inc"
} // namespace llhd
} // namespace circt

using namespace circt;
using namespace circt::llhd;
using namespace mlir;

namespace {

static bool isSuspendFree(CoroutineOp coroutine) {
  bool hasReturn = false;
  WalkResult result = coroutine.getBody().walk([&](Operation *op) {
    if (isa<ReturnOp>(op))
      hasReturn = true;
    if (isa<WaitOp, HaltOp, CallCoroutineOp>(op))
      return WalkResult::interrupt();
    return WalkResult::advance();
  });
  return hasReturn && !result.wasInterrupted();
}

struct InlineSuspendFreeCoroutinesPass
    : public llhd::impl::InlineSuspendFreeCoroutinesPassBase<
          InlineSuspendFreeCoroutinesPass> {
  void runOnOperation() override;
};

} // namespace

void InlineSuspendFreeCoroutinesPass::runOnOperation() {
  ModuleOp module = getOperation();
  SymbolTableCollection symbolTables;
  SmallVector<CallCoroutineOp> calls;
  module.walk([&](CallCoroutineOp call) { calls.push_back(call); });

  for (CallCoroutineOp call : calls) {
    auto coroutine = symbolTables.lookupNearestSymbolFrom<CoroutineOp>(
        call, call.getCalleeAttr());
    if (!coroutine || !isSuspendFree(coroutine))
      continue;

    Block &entry = coroutine.getBody().front();
    if (entry.getNumArguments() != call.getNumOperands()) {
      call.emitError("cannot inline coroutine with mismatched arguments");
      signalPassFailure();
      return;
    }

    Block *callBlock = call->getBlock();
    Region *callerRegion = callBlock->getParent();
    Block *continuation = callBlock->splitBlock(call->getIterator());

    IRMapping mapping;
    mapping.map(entry.getArguments(), call.getOperands());

    // Create and map all blocks before cloning any operations so branch
    // destinations can be remapped regardless of their source order.
    for (Block &source : coroutine.getBody()) {
      auto *cloned = new Block;
      callerRegion->getBlocks().insert(Region::iterator(continuation), cloned);
      mapping.map(&source, cloned);
      if (&source == &entry)
        continue;
      for (BlockArgument argument : source.getArguments()) {
        BlockArgument clonedArgument =
            cloned->addArgument(argument.getType(), argument.getLoc());
        mapping.map(argument, clonedArgument);
      }
    }

    for (Type resultType : call.getResultTypes())
      continuation->addArgument(resultType, call.getLoc());
    for (auto [result, argument] :
         llvm::zip(call.getResults(), continuation->getArguments()))
      result.replaceAllUsesWith(argument);

    for (Block &source : coroutine.getBody()) {
      Block *cloned = mapping.lookup(&source);
      OpBuilder builder = OpBuilder::atBlockEnd(cloned);
      for (Operation &op : source) {
        if (auto returnOp = dyn_cast<ReturnOp>(op)) {
          SmallVector<Value> results;
          for (Value value : returnOp.getOperands())
            results.push_back(mapping.lookupOrDefault(value));
          cf::BranchOp::create(builder, returnOp.getLoc(), continuation,
                               results);
        } else {
          builder.clone(op, mapping);
        }
      }
    }

    OpBuilder builder = OpBuilder::atBlockEnd(callBlock);
    cf::BranchOp::create(builder, call.getLoc(), mapping.lookup(&entry));
    call.erase();
    ++numInlined;
  }

  SmallVector<CoroutineOp> coroutines;
  module.walk([&](CoroutineOp coroutine) { coroutines.push_back(coroutine); });
  for (CoroutineOp coroutine : coroutines) {
    if (!isSuspendFree(coroutine))
      continue;
    if (!SymbolTable::symbolKnownUseEmpty(coroutine.getSymNameAttr(), module))
      continue;
    coroutine.erase();
    ++numErased;
  }
}
