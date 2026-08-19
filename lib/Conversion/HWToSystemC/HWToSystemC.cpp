//===- HWToSystemC.cpp - HW To SystemC Conversion Pass --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the main HW to SystemC Conversion Pass Implementation.
//
//===----------------------------------------------------------------------===//

#include "circt/Conversion/HWToSystemC.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/HWPasses.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/SystemC/SystemCOps.h"
#include "mlir/Dialect/EmitC/IR/EmitC.h"
#include "mlir/IR/BuiltinDialect.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TypeSwitch.h"

#include <string>
#include <type_traits>

namespace circt {
#define GEN_PASS_DEF_CONVERTHWTOSYSTEMC
#include "circt/Conversion/Passes.h.inc"
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace hw;
using namespace systemc;

//===----------------------------------------------------------------------===//
// Operation Conversion Patterns
//===----------------------------------------------------------------------===//

namespace {

/// This works on each HW module, creates corresponding SystemC modules, moves
/// the body of the module into the new SystemC module by splitting up the body
/// into field declarations, initializations done in a newly added systemc.ctor,
/// and internal methods to be registered in the constructor.
struct ConvertHWModule : public OpConversionPattern<HWModuleOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(HWModuleOp module, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Parameterized modules are supported yet.
    if (!module.getParameters().empty())
      return emitError(module->getLoc(), "module parameters not supported yet");

    auto ports = module.getPortList();
    if (llvm::any_of(ports, [](auto &port) { return port.isInOut(); }))
      return emitError(module->getLoc(), "inout arguments not supported yet");

    // Create the SystemC module.
    for (size_t i = 0; i < ports.size(); ++i)
      ports[i].type = typeConverter->convertType(ports[i].type);

    auto scModule = SCModuleOp::create(rewriter, module.getLoc(),
                                       module.getNameAttr(), ports);
    auto *outputOp = module.getBodyBlock()->getTerminator();
    scModule.setVisibility(module.getVisibility());

    auto portAttrs = module.getAllPortAttrs();
    if (!portAttrs.empty())
      scModule.setAllArgAttrs(portAttrs);

    // Create a systemc.func operation inside the module after the ctor.
    // TODO: implement logic to extract a better name and properly unique it.
    rewriter.setInsertionPointToStart(scModule.getBodyBlock());
    auto scFunc = SCFuncOp::create(rewriter, module.getLoc(),
                                   rewriter.getStringAttr("innerLogic"));

    // Inline the HW module body into the systemc.func body.
    // TODO: do some dominance analysis to detect use-before-def and cycles in
    // the use chain, which are allowed in graph regions but not in SSACFG
    // regions, and when possible fix them.
    scFunc.getBodyBlock()->erase();
    Region &scFuncBody = scFunc.getBody();
    rewriter.inlineRegionBefore(module.getBody(), scFuncBody, scFuncBody.end());

    // Register the systemc.func inside the systemc.ctor
    rewriter.setInsertionPointToStart(
        scModule.getOrCreateCtor(rewriter).getBodyBlock());
    MethodOp::create(rewriter, scModule.getLoc(), scFunc.getHandle());

    // Register the sensitivities of above SC_METHOD registration.
    SmallVector<Value> sensitivityValues(
        llvm::make_filter_range(scModule.getArguments(), [](BlockArgument arg) {
          return !isa<OutputType>(arg.getType());
        }));
    if (!sensitivityValues.empty())
      SensitiveOp::create(rewriter, scModule.getLoc(), sensitivityValues);

    // Move the block arguments of the systemc.func (that we got from the
    // hw.module) to the systemc.module
    rewriter.setInsertionPointToStart(scFunc.getBodyBlock());
    auto portsLocal = module.getPortList();
    for (size_t i = 0, e = scFunc.getRegion().getNumArguments(); i < e; ++i) {
      auto inputRead = SignalReadOp::create(rewriter, scFunc.getLoc(),
                                            scModule.getArgument(i))
                           .getResult();
      auto converted = typeConverter->materializeSourceConversion(
          rewriter, scModule.getLoc(), portsLocal[i].type, inputRead);
      scFuncBody.getArgument(0).replaceAllUsesWith(converted);
      scFuncBody.eraseArgument(0);
    }

    // Erase the HW module.
    rewriter.eraseOp(module);

    SmallVector<Value> outPorts;
    for (auto val : scModule.getArguments()) {
      if (isa<OutputType>(val.getType()))
        outPorts.push_back(val);
    }

    rewriter.setInsertionPoint(outputOp);
    for (auto args : llvm::zip(outPorts, outputOp->getOperands())) {
      Value portValue = std::get<0>(args);
      auto converted = typeConverter->materializeTargetConversion(
          rewriter, scModule.getLoc(), getSignalBaseType(portValue.getType()),
          std::get<1>(args));
      SignalWriteOp::create(rewriter, outputOp->getLoc(), portValue, converted);
    }

    // Erase the HW OutputOp.
    outputOp->dropAllReferences();
    rewriter.eraseOp(outputOp);

    return success();
  }
};

/// Convert hw.instance operations to systemc.instance.decl and a
/// systemc.instance.bind_port operation for each port in the constructor. Also
/// insert the necessary intermediate signals and write or read their state in
/// the update function accordingly.
class ConvertInstance : public OpConversionPattern<InstanceOp> {
  using OpConversionPattern::OpConversionPattern;

private:
  template <typename PortTy>
  LogicalResult
  collectPortInfo(ValueRange ports, ArrayAttr portNames,
                  SmallVector<systemc::ModuleType::PortInfo> &portInfo) const {
    for (auto inPort : llvm::zip(ports, portNames)) {
      Type ty = std::get<0>(inPort).getType();
      systemc::ModuleType::PortInfo info;

      if (isa<hw::InOutType>(ty))
        return failure();

      info.type = typeConverter->convertType(PortTy::get(ty));
      info.name = cast<StringAttr>(std::get<1>(inPort));
      portInfo.push_back(info);
    }

    return success();
  }

public:
  LogicalResult
  matchAndRewrite(InstanceOp instanceOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Make sure the parent is already converted such that we already have a
    // constructor and update function to insert operations into.
    auto scModule = instanceOp->getParentOfType<SCModuleOp>();
    if (!scModule)
      return rewriter.notifyMatchFailure(instanceOp,
                                         "parent was not an SCModuleOp");

    // Track the insertion points for the different places we need to insert
    // operations while continuing to use the active pattern rewriter.
    auto ctor = scModule.getOrCreateCtor(rewriter);
    OpBuilder::InsertPoint stateInsertPt(ctor->getBlock(),
                                         Block::iterator(ctor.getOperation()));
    OpBuilder::InsertPoint initInsertPt(ctor.getBodyBlock(),
                                        ctor.getBodyBlock()->end());

    // Collect the port types and names of the instantiated module and convert
    // them to appropriate systemc types.
    SmallVector<systemc::ModuleType::PortInfo> portInfo;
    if (failed(collectPortInfo<InputType>(adaptor.getInputs(),
                                          adaptor.getArgNames(), portInfo)) ||
        failed(collectPortInfo<OutputType>(instanceOp->getResults(),
                                           adaptor.getResultNames(), portInfo)))
      return instanceOp->emitOpError("inout ports not supported");

    Location loc = instanceOp->getLoc();
    auto instanceName = instanceOp.getInstanceNameAttr();
    auto instModuleName = instanceOp.getModuleNameAttr();

    // Declare the instance.
    rewriter.restoreInsertionPoint(stateInsertPt);
    auto instDecl = InstanceDeclOp::create(rewriter, loc, instanceName,
                                           instModuleName, portInfo);

    // Bind the input ports.
    for (size_t i = 0, numInputs = adaptor.getInputs().size(); i < numInputs;
         ++i) {
      Value input = adaptor.getInputs()[i];
      auto portId = rewriter.getIndexAttr(i);
      StringAttr signalName = rewriter.getStringAttr(
          instanceName.getValue() + "_" + portInfo[i].name.getValue());

      // Look through materialized conversions and unrealized casts to
      // recover the read of the channel this input is fed from.
      Operation *definingOp = input.getDefiningOp();
      while (definingOp && (isa<ConvertOp>(definingOp) ||
                            isa<UnrealizedConversionCastOp>(definingOp)))
        definingOp = definingOp->getOperand(0).getDefiningOp();

      if (auto readOp = dyn_cast_or_null<SignalReadOp>(definingOp)) {
        // Use the read channel directly without adding an intermediate
        // signal, unless the channel is a module input port itself. In that
        // case keep the explicit signal so that the port-to-instance data
        // path is visible in the module body.
        if (!llvm::is_contained(scModule.getArguments(), readOp.getInput())) {
          rewriter.restoreInsertionPoint(initInsertPt);
          BindPortOp::create(rewriter, loc, instDecl, portId,
                             readOp.getInput());
          continue;
        }
      }

      // Otherwise, create an intermediate signal to bind the instance port to.
      Type sigType = SignalType::get(getSignalBaseType(portInfo[i].type));
      rewriter.restoreInsertionPoint(stateInsertPt);
      Value channel = SignalOp::create(rewriter, loc, sigType, signalName);
      rewriter.restoreInsertionPoint(initInsertPt);
      BindPortOp::create(rewriter, loc, instDecl, portId, channel);
      rewriter.setInsertionPoint(instanceOp);
      SignalWriteOp::create(rewriter, loc, channel, input);
    }

    // Bind the output ports.
    for (size_t i = 0, numOutputs = instanceOp->getNumResults(); i < numOutputs;
         ++i) {
      size_t numInputs = adaptor.getInputs().size();
      Value output = instanceOp->getResult(i);
      auto portId = rewriter.getIndexAttr(i + numInputs);
      StringAttr signalName =
          rewriter.getStringAttr(instanceName.getValue() + "_" +
                                 portInfo[i + numInputs].name.getValue());

      if (output.hasOneUse()) {
        if (auto writeOp = dyn_cast<SignalWriteOp>(*output.user_begin())) {
          // Use the channel written to directly. When there are multiple
          // channels this value is written to or it is used somewhere else, we
          // cannot shortcut it and have to insert an intermediate value because
          // we cannot insert multiple bind statements for one submodule port.
          // It is also necessary to bind it to an intermediate signal when it
          // has no uses as every port has to be bound to a channel.
          rewriter.restoreInsertionPoint(initInsertPt);
          BindPortOp::create(rewriter, loc, instDecl, portId,
                             writeOp.getDest());
          writeOp->erase();
          continue;
        }
      }

      // Otherwise, create an intermediate signal.
      Type sigType =
          SignalType::get(getSignalBaseType(portInfo[i + numInputs].type));
      rewriter.restoreInsertionPoint(stateInsertPt);
      Value channel = SignalOp::create(rewriter, loc, sigType, signalName);
      rewriter.restoreInsertionPoint(initInsertPt);
      BindPortOp::create(rewriter, loc, instDecl, portId, channel);
      rewriter.setInsertionPoint(instanceOp);
      auto instOut = SignalReadOp::create(rewriter, loc, channel);
      // Convert the read value back to the original integer type, since
      // operations like comb.* require signless integers.
      auto converted = typeConverter->materializeSourceConversion(
          rewriter, loc, instanceOp->getResultTypes()[i], instOut.getResult());
      output.replaceAllUsesWith(converted ? converted : instOut.getResult());
    }

    rewriter.eraseOp(instanceOp);
    return success();
  }
};

/// Remove the Seq clock wrapper after its type has been converted to i1.
template <typename OpTy>
struct ConvertClockCast : public OpConversionPattern<OpTy> {
  using OpConversionPattern<OpTy>::OpConversionPattern;
  using OpAdaptor = typename OpConversionPattern<OpTy>::OpAdaptor;

  LogicalResult
  matchAndRewrite(OpTy op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOp(op, adaptor.getInput());
    return success();
  }
};

/// Walk through the casts introduced for a module input and recover the
/// SystemC channel that carries a clock.
static Value findClockChannel(Value value) {
  while (Operation *definingOp = value.getDefiningOp()) {
    if (auto convert = dyn_cast<ConvertOp>(definingOp)) {
      value = convert.getInput();
      continue;
    }
    if (auto toClock = dyn_cast<seq::ToClockOp>(definingOp)) {
      value = toClock.getInput();
      continue;
    }
    if (auto read = dyn_cast<SignalReadOp>(definingOp))
      return read.getInput();
    break;
  }
  return {};
}

static StringAttr getUniqueStateName(SCModuleOp module, StringRef requested,
                                     Builder &builder) {
  llvm::StringSet<> names;
  for (Attribute attr : module.getPortNames())
    names.insert(cast<StringAttr>(attr).getValue());
  for (auto nameDecl : module.getBodyBlock()->getOps<SignalOp>())
    names.insert(nameDecl.getName());

  std::string base = requested.empty() ? "state" : requested.str();
  base += "_state";
  std::string candidate = base;
  for (unsigned suffix = 0; names.contains(candidate); ++suffix)
    candidate = base + "_" + std::to_string(suffix);
  return builder.getStringAttr(candidate);
}

/// Lower a Seq register into a SystemC signal updated from the module's
/// existing SC_METHOD. The method is sensitive to the state signal as well as
/// all input ports. A `posedge()` query guards the state update, preserving
/// simultaneous-register semantics while avoiding a second process and the
/// associated cross-region cloning of the register's input cone.
template <typename OpTy>
struct ConvertCompReg : public OpConversionPattern<OpTy> {
  using OpConversionPattern<OpTy>::OpConversionPattern;
  using OpAdaptor = typename OpConversionPattern<OpTy>::OpAdaptor;

  LogicalResult
  matchAndRewrite(OpTy reg, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (reg.getInitialValue())
      return rewriter.notifyMatchFailure(reg,
                                         "initial values are not supported");

    auto scModule = reg->template getParentOfType<SCModuleOp>();
    if (!scModule)
      return rewriter.notifyMatchFailure(reg, "parent is not an SCModuleOp");
    auto scFunc = reg->template getParentOfType<SCFuncOp>();
    if (!scFunc)
      return rewriter.notifyMatchFailure(reg, "parent is not an SCFuncOp");

    Value clockChannel = findClockChannel(adaptor.getClk());
    if (!clockChannel)
      return rewriter.notifyMatchFailure(
          reg, "clock is not read directly from a SystemC channel");

    Location loc = reg.getLoc();
    Type stateType = this->getTypeConverter()->convertType(reg.getType());
    auto signalType = SignalType::get(stateType);
    StringRef requestedName = reg.getName().value_or("");
    StringAttr stateName =
        getUniqueStateName(scModule, requestedName, rewriter);

    auto ctor = scModule.getOrCreateCtor(rewriter);
    rewriter.setInsertionPoint(ctor);
    Value state =
        SignalOp::create(rewriter, loc, signalType, stateName).getSignal();

    // Re-run the method after the signal update delta cycle so combinational
    // outputs observe the newly committed register value.
    SensitiveOp sensitivity;
    for (auto candidate : ctor.getBodyBlock()->template getOps<SensitiveOp>())
      sensitivity = candidate;
    if (sensitivity)
      sensitivity.getSensitivitiesMutable().append(state);
    else {
      rewriter.setInsertionPointToStart(ctor.getBodyBlock());
      SensitiveOp::create(rewriter, loc, ValueRange{state});
    }

    rewriter.setInsertionPointToStart(scFunc.getBodyBlock());
    Value stateRead = SignalReadOp::create(rewriter, loc, state);
    Value current = this->getTypeConverter()->materializeSourceConversion(
        rewriter, loc, reg.getType(), stateRead);

    rewriter.setInsertionPointToEnd(scFunc.getBodyBlock());
    Value next = reg.getInput();
    if constexpr (std::is_same_v<OpTy, seq::CompRegClockEnabledOp>)
      next = comb::MuxOp::create(rewriter, loc, reg.getClockEnable(), next,
                                 current);
    if (reg.getReset())
      next = comb::MuxOp::create(rewriter, loc, reg.getReset(),
                                 reg.getResetValue(), next);

    Value posedge = SignalPosedgeOp::create(rewriter, loc, clockChannel);
    next = comb::MuxOp::create(rewriter, loc, posedge, next, current);
    Value converted = this->getTypeConverter()->materializeTargetConversion(
        rewriter, loc, stateType, next);
    SignalWriteOp::create(rewriter, loc, state, converted);

    rewriter.replaceOp(reg, current);
    return success();
  }
};

/// Lower the FIRRTL-flavored register operation. Synchronous reset is sampled
/// only on a positive clock edge; asynchronous reset is selected outside the
/// edge guard so a reset-port event updates the state immediately.
struct ConvertFirReg : public OpConversionPattern<seq::FirRegOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(seq::FirRegOp reg, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (reg.hasPresetValue())
      return rewriter.notifyMatchFailure(reg,
                                         "preset values are not supported");

    auto scModule = reg->getParentOfType<SCModuleOp>();
    if (!scModule)
      return rewriter.notifyMatchFailure(reg, "parent is not an SCModuleOp");
    auto scFunc = reg->getParentOfType<SCFuncOp>();
    if (!scFunc)
      return rewriter.notifyMatchFailure(reg, "parent is not an SCFuncOp");

    Value clockChannel = findClockChannel(adaptor.getClk());
    if (!clockChannel)
      return rewriter.notifyMatchFailure(
          reg, "clock is not read directly from a SystemC channel");

    Location loc = reg.getLoc();
    Type stateType = getTypeConverter()->convertType(reg.getType());
    auto signalType = SignalType::get(stateType);
    StringAttr stateName =
        getUniqueStateName(scModule, reg.getName(), rewriter);

    auto ctor = scModule.getOrCreateCtor(rewriter);
    rewriter.setInsertionPoint(ctor);
    Value state =
        SignalOp::create(rewriter, loc, signalType, stateName).getSignal();

    SensitiveOp sensitivity;
    for (auto candidate : ctor.getBodyBlock()->getOps<SensitiveOp>())
      sensitivity = candidate;
    if (sensitivity)
      sensitivity.getSensitivitiesMutable().append(state);
    else {
      rewriter.setInsertionPointToStart(ctor.getBodyBlock());
      SensitiveOp::create(rewriter, loc, ValueRange{state});
    }

    rewriter.setInsertionPointToStart(scFunc.getBodyBlock());
    Value stateRead = SignalReadOp::create(rewriter, loc, state);
    Value current = getTypeConverter()->materializeSourceConversion(
        rewriter, loc, reg.getType(), stateRead);

    rewriter.setInsertionPointToEnd(scFunc.getBodyBlock());
    Value posedge = SignalPosedgeOp::create(rewriter, loc, clockChannel);

    Value next = reg.getNext();
    if (reg.getReset() && !reg.getIsAsync())
      next = comb::MuxOp::create(rewriter, loc, reg.getReset(),
                                 reg.getResetValue(), next);
    next = comb::MuxOp::create(rewriter, loc, posedge, next, current);
    if (reg.getReset() && reg.getIsAsync())
      next = comb::MuxOp::create(rewriter, loc, reg.getReset(),
                                 reg.getResetValue(), next);

    Value converted = getTypeConverter()->materializeTargetConversion(
        rewriter, loc, stateType, next);
    SignalWriteOp::create(rewriter, loc, state, converted);
    rewriter.replaceOp(reg, current);
    return success();
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// Conversion Infrastructure
//===----------------------------------------------------------------------===//

static void populateLegality(ConversionTarget &target) {
  target.addIllegalDialect<HWDialect>();
  target.addLegalDialect<mlir::BuiltinDialect>();
  target.addLegalDialect<systemc::SystemCDialect>();
  target.addLegalDialect<comb::CombDialect>();
  target.addLegalDialect<emitc::EmitCDialect>();
  target.addIllegalDialect<seq::SeqDialect>();
  target.addLegalOp<hw::ConstantOp>();
}

static void populateOpConversion(RewritePatternSet &patterns,
                                 TypeConverter &typeConverter) {
  patterns
      .add<ConvertHWModule, ConvertInstance, ConvertClockCast<seq::ToClockOp>,
           ConvertClockCast<seq::FromClockOp>, ConvertCompReg<seq::CompRegOp>,
           ConvertCompReg<seq::CompRegClockEnabledOp>, ConvertFirReg>(
          typeConverter, patterns.getContext());
}

static void populateTypeConversion(TypeConverter &converter) {
  converter.addConversion([](Type type) { return type; });
  converter.addConversion([&](SignalType type) {
    return SignalType::get(converter.convertType(type.getBaseType()));
  });
  converter.addConversion([&](InputType type) {
    return InputType::get(converter.convertType(type.getBaseType()));
  });
  converter.addConversion([&](systemc::InOutType type) {
    return systemc::InOutType::get(converter.convertType(type.getBaseType()));
  });
  converter.addConversion([&](OutputType type) {
    return OutputType::get(converter.convertType(type.getBaseType()));
  });
  converter.addConversion([](IntegerType type) -> Type {
    auto bw = type.getIntOrFloatBitWidth();
    if (bw == 1)
      return type;

    if (bw <= 64) {
      if (type.isSigned())
        return systemc::IntType::get(type.getContext(), bw);

      return UIntType::get(type.getContext(), bw);
    }

    if (bw <= 512) {
      if (type.isSigned())
        return BigIntType::get(type.getContext(), bw);

      return BigUIntType::get(type.getContext(), bw);
    }

    return BitVectorType::get(type.getContext(), bw);
  });
  converter.addConversion([](seq::ClockType type) -> Type {
    return IntegerType::get(type.getContext(), 1);
  });

  converter.addSourceMaterialization(
      [](OpBuilder &builder, Type type, ValueRange values, Location loc) {
        assert(values.size() == 1);
        auto op = ConvertOp::create(builder, loc, type, values[0]);
        return op.getResult();
      });

  converter.addTargetMaterialization(
      [](OpBuilder &builder, Type type, ValueRange values, Location loc) {
        assert(values.size() == 1);
        auto op = ConvertOp::create(builder, loc, type, values[0]);
        return op.getResult();
      });
}

//===----------------------------------------------------------------------===//
// HW to SystemC Conversion Pass
//===----------------------------------------------------------------------===//

namespace {
struct HWToSystemCPass
    : public circt::impl::ConvertHWToSystemCBase<HWToSystemCPass> {
  void runOnOperation() override;
};
} // namespace

/// Create a HW to SystemC dialects conversion pass.
std::unique_ptr<OperationPass<ModuleOp>> circt::createConvertHWToSystemCPass() {
  return std::make_unique<HWToSystemCPass>();
}

/// This is the main entrypoint for the HW to SystemC conversion pass.
void HWToSystemCPass::runOnOperation() {
  MLIRContext &context = getContext();
  ModuleOp module = getOperation();

  // Prepare the modules for the conversion. The conversion only supports
  // scalar ports and combinational operations, so flatten aggregate ports,
  // lower aggregate operations to comb ops, and convert the remaining
  // bitcasts. Port names use '_' as the join character to keep the generated
  // C++ identifiers valid.
  mlir::OpPassManager preparePM("builtin.module");
  preparePM.addPass(hw::createFlattenIO(hw::FlattenIOOptions{true, false, '_'}));
  auto &modulePM = preparePM.nestAny();
  modulePM.addPass(hw::createHWAggregateToComb());
  preparePM.addPass(hw::createHWConvertBitcasts());
  // hw-convert-bitcasts may reintroduce aggregate ops, so lower them once
  // more.
  preparePM.nestAny().addPass(hw::createHWAggregateToComb());
  if (failed(runPipeline(preparePM, module)))
    return signalPassFailure();

  // The aggregate preparation may leave dead bitcasts behind. Erase trivially
  // dead operations so the full conversion below doesn't trip over them.
  // (mlir's remove-dead-values pass is not used here: it would replace the
  // uses of dead values with ub.poison, which the SystemC flow doesn't
  // support.)
  bool changed = true;
  while (changed) {
    changed = false;
    module->walk([&](Operation *op) {
      if (op->use_empty() && mlir::wouldOpBeTriviallyDead(op)) {
        op->erase();
        changed = true;
      }
    });
  }

  // Create the include operation here to have exactly one 'systemc' include at
  // the top instead of one per module.
  OpBuilder builder(module.getRegion());
  emitc::IncludeOp::create(builder, module->getLoc(), "systemc.h", true);

  ConversionTarget target(context);
  TypeConverter typeConverter;
  RewritePatternSet patterns(&context);
  populateLegality(target);
  populateTypeConversion(typeConverter);
  populateOpConversion(patterns, typeConverter);

  if (failed(applyFullConversion(module, target, std::move(patterns))))
    signalPassFailure();
}
