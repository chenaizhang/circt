//===- SystemCContainerInteropLowering.cpp - Lower container-side interop -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Interop/InteropOps.h"
#include "circt/Dialect/SystemC/SystemCOpInterfaces.h"
#include "circt/Dialect/SystemC/SystemCOps.h"
#include "circt/Dialect/SystemC/SystemCPasses.h"
#include "circt/Support/Namespace.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

namespace circt {
namespace systemc {
#define GEN_PASS_DEF_SYSTEMCCONTAINERINTEROPLOWERING
#include "circt/Dialect/SystemC/Passes.h.inc"
} // namespace systemc
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace circt::systemc;
using namespace circt::interop;

namespace {
struct SCModuleNamespace : Namespace {
  SCModuleNamespace() = default;
  explicit SCModuleNamespace(SCModuleOp module) { add(module); }

  void add(SCModuleOp module) {
    for (auto portName : module.getPortNames())
      nextIndex.insert({mlir::cast<StringAttr>(portName).getValue(), 0});

    module->walk([&](SystemCNameDeclOpInterface op) {
      nextIndex.insert({op.getName(), 0});
    });
  }
};

template <typename OpTy>
class UniquingOpConversionPattern : public OpConversionPattern<OpTy> {
public:
  UniquingOpConversionPattern(SCModuleNamespace &uniquer, MLIRContext *context,
                              PatternBenefit benefit = 1)
      : OpConversionPattern<OpTy>(context, benefit), nameUniquer(uniquer) {}

protected:
  SCModuleNamespace &nameUniquer;
};

class ProceduralAllocOpConversion
    : public UniquingOpConversionPattern<ProceduralAllocOp> {
public:
  using UniquingOpConversionPattern<
      ProceduralAllocOp>::UniquingOpConversionPattern;

  LogicalResult
  matchAndRewrite(ProceduralAllocOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Operation *interopParent = op->getParentWithTrait<HasInterop>();
    auto module = dyn_cast_or_null<SCModuleOp>(interopParent);
    if (!module)
      return failure();

    auto stateBuilder = OpBuilder::atBlockBegin(module.getBodyBlock());
    SmallVector<Value> variables;
    for (Value state : op.getStates()) {
      variables.push_back(stateBuilder.create<VariableOp>(
          op.getLoc(), state.getType(),
          StringAttr::get(rewriter.getContext(),
                          nameUniquer.newName("interopState")),
          Value()));
    }

    rewriter.replaceOp(op, variables);
    return success();
  }
};

class ProceduralInitOpConversion
    : public OpConversionPattern<ProceduralInitOp> {
public:
  using OpConversionPattern<ProceduralInitOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ProceduralInitOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto module = dyn_cast_or_null<SCModuleOp>(
        op->getParentWithTrait<HasInterop>());
    if (!module)
      return failure();

    auto ctor = module.getOrCreateCtor(rewriter);
    rewriter.mergeBlocks(op.getBody(), &ctor.getBody().front(), {});
    auto &ctorBody = ctor.getBody().front();
    auto initBuilder = OpBuilder::atBlockEnd(&ctorBody);
    auto returnOp = dyn_cast<interop::ReturnOp>(&ctorBody.back());
    if (!returnOp)
      return op.emitError("expected interop.return at the end of the init body");

    for (auto [state, value] : llvm::zip(adaptor.getStates(),
                                         returnOp.getReturnValues()))
      AssignOp::create(initBuilder, module.getLoc(), state, value);

    rewriter.eraseOp(returnOp);
    rewriter.eraseOp(op);
    return success();
  }
};

class ProceduralUpdateOpConversion
    : public OpConversionPattern<ProceduralUpdateOp> {
public:
  using OpConversionPattern<ProceduralUpdateOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ProceduralUpdateOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto module = dyn_cast_or_null<SCModuleOp>(
        op->getParentWithTrait<HasInterop>());
    if (!module)
      return failure();

    auto returnOp = dyn_cast<interop::ReturnOp>(op.getBody()->getTerminator());
    if (!returnOp)
      return op.emitError("expected interop.return at the end of the update body");

    SmallVector<Value> replacements(adaptor.getStates());
    llvm::append_range(replacements, adaptor.getInputs());
    rewriter.inlineBlockBefore(op.getBody(), op, replacements);
    rewriter.replaceOp(op, returnOp.getReturnValues());
    rewriter.eraseOp(returnOp);
    return success();
  }
};

class ProceduralDeallocOpConversion
    : public OpConversionPattern<ProceduralDeallocOp> {
public:
  using OpConversionPattern<ProceduralDeallocOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ProceduralDeallocOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto module = dyn_cast_or_null<SCModuleOp>(
        op->getParentWithTrait<HasInterop>());
    if (!module)
      return failure();

    rewriter.mergeBlocks(op.getBody(),
                         &module.getOrCreateDestructor().getBody().front(),
                         adaptor.getStates());
    rewriter.eraseOp(op);
    return success();
  }
};
} // namespace

namespace {
struct SystemCContainerInteropLoweringPass
    : circt::systemc::impl::SystemCContainerInteropLoweringBase<
          SystemCContainerInteropLoweringPass> {
  void runOnOperation() override;
  void getDependentDialects(DialectRegistry &registry) const override;
};
} // namespace

void SystemCContainerInteropLoweringPass::getDependentDialects(
    DialectRegistry &registry) const {
  registry.insert<SystemCDialect, interop::InteropDialect,
                  emitc::EmitCDialect, func::FuncDialect>();
}

void SystemCContainerInteropLoweringPass::runOnOperation() {
  RewritePatternSet patterns(&getContext());

  ConversionTarget target(getContext());
  target.addLegalDialect<emitc::EmitCDialect>();
  target.addLegalDialect<SystemCDialect>();
  target.addLegalOp<CallIndirectOp>();
  target.addDynamicallyLegalOp<ProceduralAllocOp, ProceduralInitOp,
                               ProceduralUpdateOp, ProceduralDeallocOp>(
      [](Operation *op) {
        if (auto *parent = op->getParentWithTrait<HasInterop>())
          return !isa<SCModuleOp>(parent);
        return false;
      });

  SCModuleNamespace nameUniquer;
  getOperation()->walk([&](SCModuleOp module) {
    nameUniquer.add(module);
  });

  patterns.add<ProceduralAllocOpConversion>(nameUniquer, &getContext());
  patterns.add<ProceduralInitOpConversion, ProceduralUpdateOpConversion,
               ProceduralDeallocOpConversion>(&getContext());

  if (failed(applyPartialConversion(getOperation(), target,
                                    std::move(patterns))))
    signalPassFailure();
}

std::unique_ptr<Pass>
circt::systemc::createSystemCContainerInteropLoweringPass() {
  return std::make_unique<SystemCContainerInteropLoweringPass>();
}
