//===- FlattenIO.cpp - HW I/O flattening pass -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/HWPasses.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>

namespace circt {
namespace hw {
#define GEN_PASS_DEF_FLATTENIO
#include "circt/Dialect/HW/Passes.h.inc"
} // namespace hw
} // namespace circt

using namespace mlir;
using namespace circt;

namespace {
/// Set by the pass driver for the duration of a run; the array-aware helpers
/// consult it so that array flattening stays an opt-in behavior.
bool flattenArraysEnabled = false;
} // namespace

static bool isStructType(Type type) {
  return isa<hw::StructType>(hw::getCanonicalType(type));
}

static hw::StructType getStructType(Type type) {
  return dyn_cast<hw::StructType>(hw::getCanonicalType(type));
}

static bool isArrayType(Type type) {
  return isa<hw::ArrayType>(hw::getCanonicalType(type));
}

static hw::ArrayType getArrayType(Type type) {
  return dyn_cast<hw::ArrayType>(hw::getCanonicalType(type));
}

static bool flattenArrays() { return flattenArraysEnabled; }

static bool isAggregateType(Type type) {
  return isStructType(type) || (flattenArrays() && isArrayType(type));
}

// Legal if no in- or output type is an aggregate (struct or array).
static bool isLegalModLikeOp(hw::HWModuleLike moduleLikeOp) {
  return llvm::none_of(moduleLikeOp.getHWModuleType().getPortTypes(),
                       isAggregateType);
}

static llvm::SmallVector<Type> getInnerTypes(hw::StructType t) {
  llvm::SmallVector<Type> inner;
  t.getInnerTypes(inner);
  for (auto [index, innerType] : llvm::enumerate(inner))
    inner[index] = hw::getCanonicalType(innerType);
  return inner;
}

/// Explode an aggregate value into its immediate child values. Structs are
/// exploded via `hw.struct_explode`; arrays via constant-index `hw.array_get`.
static SmallVector<Value> explodeAggregate(OpBuilder &builder, Location loc,
                                           Value value) {
  SmallVector<Value> elements;
  if (auto structType = getStructType(value.getType())) {
    auto explodeOp = hw::StructExplodeOp::create(
        builder, loc, getInnerTypes(structType), value);
    llvm::copy(explodeOp.getResults(), std::back_inserter(elements));
  } else if (flattenArrays()) {
    auto arrayType = getArrayType(value.getType());
    auto indexWidth =
        std::max(1u, llvm::Log2_64_Ceil(arrayType.getNumElements()));
    for (uint64_t i = 0; i < arrayType.getNumElements(); ++i) {
      auto index = builder.create<hw::ConstantOp>(loc, APInt(indexWidth, i));
      elements.push_back(
          hw::ArrayGetOp::create(builder, loc, value, index).getResult());
    }
  }
  return elements;
}

/// Implode scalar values into an aggregate of the given (canonical) type.
/// Array elements are passed in index order; `hw.array_create` takes its
/// operands MSB-first, so they are reversed here.
static Value implodeAggregate(OpBuilder &builder, Location loc, Type type,
                              ValueRange elements) {
  if (auto structType = getStructType(type))
    return hw::StructCreateOp::create(builder, loc, structType, elements)
        .getResult();
  auto arrayType = flattenArrays() ? getArrayType(type) : hw::ArrayType();
  assert(arrayType && "expected a struct or array type");
  SmallVector<Value> reversed(elements.begin(), elements.end());
  std::reverse(reversed.begin(), reversed.end());
  return hw::ArrayCreateOp::create(builder, loc, reversed).getResult();
}

/// Returns the flattened port-name suffixes for an aggregate type: field names
/// for structs, zero-based indices for arrays.
static SmallVector<std::string> getFlattenedNameSuffixes(Type type) {
  SmallVector<std::string> suffixes;
  if (auto structType = getStructType(type))
    for (auto field : structType.getElements())
      suffixes.push_back(field.name.getValue().str());
  else if (flattenArrays())
    if (auto arrayType = getArrayType(type))
      for (uint64_t i = 0; i < arrayType.getNumElements(); ++i)
        suffixes.push_back(std::to_string(i));
  return suffixes;
}

/// Number of immediate children a type flattens into.
static size_t getFlattenedSize(Type type) {
  if (auto structType = getStructType(type))
    return structType.getElements().size();
  if (flattenArrays())
    if (auto arrayType = getArrayType(type))
      return arrayType.getNumElements();
  return 1;
}

namespace {

/// Flatten the given value ranges into a single vector of values.
static SmallVector<Value> flattenValues(ArrayRef<ValueRange> values) {
  SmallVector<Value> result;
  for (const auto &vals : values)
    llvm::append_range(result, vals);
  return result;
}

// Replaces an output op with a new output with flattened (exploded) structs.
struct OutputOpConversion : public OpConversionPattern<hw::OutputOp> {
  OutputOpConversion(TypeConverter &typeConverter, MLIRContext *context,
                     DenseSet<Operation *> *opVisited)
      : OpConversionPattern(typeConverter, context), opVisited(opVisited) {}

  LogicalResult
  matchAndRewrite(hw::OutputOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    llvm::SmallVector<Value> convOperands;

    // Flatten the operands.
    for (auto operand : adaptor.getOperands()) {
      if (isAggregateType(operand.getType()))
        llvm::append_range(convOperands,
                           explodeAggregate(rewriter, op.getLoc(), operand));
      else
        convOperands.push_back(operand);
    }

    // And replace.
    opVisited->insert(op->getParentOp());
    rewriter.replaceOpWithNewOp<hw::OutputOp>(op, convOperands);
    return success();
  }

  LogicalResult
  matchAndRewrite(hw::OutputOp op, OneToNOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    llvm::SmallVector<Value> convOperands;

    // Flatten the operands.
    for (auto operand : flattenValues(adaptor.getOperands())) {
      if (isAggregateType(operand.getType()))
        llvm::append_range(convOperands,
                           explodeAggregate(rewriter, op.getLoc(), operand));
      else
        convOperands.push_back(operand);
    }

    // And replace.
    opVisited->insert(op->getParentOp());
    rewriter.replaceOpWithNewOp<hw::OutputOp>(op, convOperands);
    return success();
  }
  DenseSet<Operation *> *opVisited;
};

struct InstanceOpConversion : public OpConversionPattern<hw::InstanceOp> {
  InstanceOpConversion(TypeConverter &typeConverter, MLIRContext *context,
                       DenseSet<hw::InstanceOp> *convertedOps,
                       const StringSet<> *externModules)
      : OpConversionPattern(typeConverter, context), convertedOps(convertedOps),
        externModules(externModules) {}

  LogicalResult
  matchAndRewrite(hw::InstanceOp op, OneToNOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto referencedMod = op.getReferencedModuleNameAttr();
    // If externModules is populated and this is an extern module instance,
    // donot flatten it.
    if (externModules->contains(referencedMod.getValue()))
      return success();

    auto loc = op.getLoc();
    // Flatten the operands.
    llvm::SmallVector<Value> convOperands;
    for (auto operand : flattenValues(adaptor.getOperands())) {
      if (isAggregateType(operand.getType()))
        llvm::append_range(convOperands,
                           explodeAggregate(rewriter, loc, operand));
      else
        convOperands.push_back(operand);
    }

    // Get the new module return type.
    llvm::SmallVector<Type> newResultTypes;
    for (auto oldResultType : op.getResultTypes()) {
      if (auto structType = getStructType(oldResultType)) {
        for (auto t : structType.getElements())
          newResultTypes.push_back(t.type);
      } else if (flattenArrays()) {
        auto arrayType = getArrayType(oldResultType);
        if (arrayType)
          for (uint64_t i = 0; i < arrayType.getNumElements(); ++i)
            newResultTypes.push_back(arrayType.getElementType());
        else
          newResultTypes.push_back(oldResultType);
      } else
        newResultTypes.push_back(oldResultType);
    }

    // Create the new instance with the flattened module, attributes will be
    // adjusted later.
    auto newInstance = hw::InstanceOp::create(
        rewriter, loc, newResultTypes, op.getInstanceNameAttr(),
        FlatSymbolRefAttr::get(referencedMod), convOperands,
        op.getArgNamesAttr(), op.getResultNamesAttr(), op.getParametersAttr(),
        op.getInnerSymAttr(), op.getDoNotPrintAttr());

    // re-create any structs and arrays in the result.
    llvm::SmallVector<Value> convResults;
    size_t oldResultCntr = 0;
    for (size_t resIndex = 0; resIndex < newInstance.getNumResults();
         ++resIndex) {
      Type oldResultType = op.getResultTypes()[oldResultCntr];
      if (isAggregateType(oldResultType)) {
        size_t nElements = getFlattenedSize(oldResultType);
        auto imploded = implodeAggregate(
            rewriter, loc, hw::getCanonicalType(oldResultType),
            newInstance.getResults().slice(resIndex, nElements));
        convResults.push_back(imploded);
        resIndex += nElements - 1;
      } else
        convResults.push_back(newInstance.getResult(resIndex));

      ++oldResultCntr;
    }
    rewriter.replaceOp(op, convResults);
    convertedOps->insert(newInstance);
    return success();
  }

  DenseSet<hw::InstanceOp> *convertedOps;
  const StringSet<> *externModules;
};

using IOTypes = std::pair<TypeRange, TypeRange>;

struct IOInfo {
  // A mapping between an arg/res index and the aggregate type of the given
  // field.
  DenseMap<unsigned, Type> argAggregates, resAggregates;

  // Records of the original arg/res types.
  SmallVector<Type> argTypes, resTypes;
};

class FlattenIOTypeConverter : public TypeConverter {
public:
  FlattenIOTypeConverter() {
    addConversion([](Type type, SmallVectorImpl<Type> &results) {
      if (auto structType = getStructType(type)) {
        for (auto field : structType.getElements())
          results.push_back(field.type);
      } else if (flattenArrays()) {
        auto arrayType = getArrayType(type);
        if (arrayType)
          for (uint64_t i = 0; i < arrayType.getNumElements(); ++i)
            results.push_back(arrayType.getElementType());
        else
          results.push_back(type);
      } else
        results.push_back(type);
      return success();
    });

    // Materialize aggregates to their children via explode / constant-index
    // gets. This situation may occur in case of hw.extern_module's with
    // aggregate outputs. Registered with the original-type overload so that
    // the produced values can be returned by value; the ValueRange overload
    // would leave a dangling view over a temporary vector.
    addTargetMaterialization([](OpBuilder &builder, TypeRange resultTypes,
                                ValueRange inputs, Location loc, Type) {
      if (inputs.size() != 1)
        return SmallVector<Value>();
      // Pass through values that already have the expected types. This also
      // covers the legalization of unrealized_conversion_cast ops created
      // during operand remapping, where the inputs are the already-converted
      // values.
      if (resultTypes.size() == inputs.size() &&
          TypeRange(ValueRange(inputs)) == resultTypes)
        return SmallVector<Value>(inputs.begin(), inputs.end());
      if (auto structType = getStructType(inputs[0].getType())) {
        if (structType.getElements().size() != resultTypes.size())
          return SmallVector<Value>();
        // Use the expected result types directly so that the explode op
        // matches the driver's expectations even for aliased field types.
        auto explodeOp =
            hw::StructExplodeOp::create(builder, loc, resultTypes, inputs[0]);
        return SmallVector<Value>(explodeOp.getResults().begin(),
                                  explodeOp.getResults().end());
      }
      if (flattenArrays()) {
        if (auto arrayType = getArrayType(inputs[0].getType())) {
          if (arrayType.getNumElements() != resultTypes.size())
            return SmallVector<Value>();
        SmallVector<Value> elements;
        auto indexWidth =
            std::max(1u, llvm::Log2_64_Ceil(arrayType.getNumElements()));
          for (uint64_t i = 0; i < arrayType.getNumElements(); ++i) {
            auto index =
                builder.create<hw::ConstantOp>(loc, APInt(indexWidth, i));
            elements.push_back(
                hw::ArrayGetOp::create(builder, loc, inputs[0], index)
                    .getResult());
          }
          return elements;
        }
      }
      return SmallVector<Value>();
    });

    addTargetMaterialization([](OpBuilder &builder, hw::StructType type,
                                ValueRange inputs, Location loc) {
      llvm::errs() << "DBG single-struct nIn=" << inputs.size() << " ty=" << type
                   << "\n";
      auto result = hw::StructCreateOp::create(builder, loc, type, inputs);
      return result.getResult();
    });
    addTargetMaterialization([](OpBuilder &builder, hw::ArrayType type,
                                ValueRange inputs, Location loc) {
      llvm::errs() << "DBG single-array nIn=" << inputs.size() << " ty=" << type
                   << "\n";
      SmallVector<Value> reversed(inputs.begin(), inputs.end());
      std::reverse(reversed.begin(), reversed.end());
      auto result = hw::ArrayCreateOp::create(builder, loc, reversed);
      return result.getResult();
    });

    addTargetMaterialization([](OpBuilder &builder, hw::TypeAliasType type,
                                ValueRange inputs, Location loc) {
      llvm::errs() << "DBG single-alias nIn=" << inputs.size() << " ty=" << type
                   << "\n";
      Value result;
      if (flattenArrays() && isArrayType(hw::getCanonicalType(type))) {
        SmallVector<Value> reversed(inputs.begin(), inputs.end());
        std::reverse(reversed.begin(), reversed.end());
        result = hw::ArrayCreateOp::create(builder, loc, reversed).getResult();
      } else {
        result = hw::StructCreateOp::create(builder, loc, type, inputs)
                     .getResult();
      }
      return result;
    });

    // In the presence of hw.extern_module which takes struct arguments, we may
    // have materialized struct explodes for said arguments (say, e.g., if the
    // parent module of the hw.instance had structs in its input, and feeds
    // these structs to the hw.instance).
    // These struct explodes needs to be converted back to the original struct,
    // which persist beyond the conversion.
    addSourceMaterialization([](OpBuilder &builder, hw::StructType type,
                                ValueRange inputs, Location loc) {
      auto result = hw::StructCreateOp::create(builder, loc, type, inputs);
      return result.getResult();
    });
    addSourceMaterialization([](OpBuilder &builder, hw::ArrayType type,
                                ValueRange inputs, Location loc) {
      SmallVector<Value> reversed(inputs.begin(), inputs.end());
      std::reverse(reversed.begin(), reversed.end());
      auto result = hw::ArrayCreateOp::create(builder, loc, reversed);
      return result.getResult();
    });
  }
};

} // namespace

template <typename... TOp>
static void addSignatureConversion(DenseMap<Operation *, IOInfo> &ioMap,
                                   ConversionTarget &target,
                                   RewritePatternSet &patterns,
                                   FlattenIOTypeConverter &typeConverter) {
  (hw::populateHWModuleLikeTypeConversionPattern(TOp::getOperationName(),
                                                 patterns, typeConverter),
   ...);

  // Legality is defined by a module having been processed once. This is due to
  // that a pattern cannot be applied multiple times (a 'pattern was already
  // applied' error - a case that would occur for nested structs). Additionally,
  // if a pattern could be applied multiple times, this would complicate
  // updating arg/res names.

  // Instead, we define legality as when a module has had a modification to its
  // top-level i/o. This ensures that only a single level of structs are
  // processed during signature conversion, which then allows us to use the
  // signature conversion in a recursive manner.
  target.addDynamicallyLegalOp<TOp...>([&](hw::HWModuleLike moduleLikeOp) {
    if (isLegalModLikeOp(moduleLikeOp))
      return true;

    // This op is involved in conversion. Check if the signature has changed.
    auto ioInfoIt = ioMap.find(moduleLikeOp);
    if (ioInfoIt == ioMap.end()) {
      // Op wasn't primed in the map. Do the safe thing, assume
      // that it's not considered in this pass, and mark it as legal
      return true;
    }
    auto ioInfo = ioInfoIt->second;

    auto compareTypes = [&](TypeRange oldTypes, TypeRange newTypes) {
      return llvm::any_of(llvm::zip(oldTypes, newTypes), [&](auto typePair) {
        auto oldType = std::get<0>(typePair);
        auto newType = std::get<1>(typePair);
        return oldType != newType;
      });
    };
    auto mtype = moduleLikeOp.getHWModuleType();
    if (compareTypes(mtype.getOutputTypes(), ioInfo.resTypes) ||
        compareTypes(mtype.getInputTypes(), ioInfo.argTypes))
      return true;

    // We're pre-conversion for an op that was primed in the map - it will
    // always be illegal since it has to-be-converted struct types at its I/O.
    return false;
  });
}

template <typename T>
static bool hasUnconvertedOps(mlir::ModuleOp module) {
  return llvm::any_of(module.getBody()->getOps<T>(),
                      [](T op) { return !isLegalModLikeOp(op); });
}

template <typename T>
static DenseMap<Operation *, IOTypes> populateIOMap(mlir::ModuleOp module) {
  DenseMap<Operation *, IOTypes> ioMap;
  for (auto op : module.getOps<T>())
    ioMap[op] = {op.getArgumentTypes(), op.getResultTypes()};
  return ioMap;
}

template <typename ModTy, typename T>
static llvm::SmallVector<Attribute>
updateNameAttribute(ModTy op, StringRef attrName,
                    DenseMap<unsigned, Type> &aggregateMap, T oldNames,
                    char joinChar) {
  llvm::SmallVector<Attribute> newNames;
  for (auto [i, oldName] : llvm::enumerate(oldNames)) {
    // Was this arg/res index an aggregate?
    auto it = aggregateMap.find(i);
    if (it == aggregateMap.end()) {
      // No, keep old name.
      newNames.push_back(StringAttr::get(op->getContext(), oldName));
      continue;
    }

    // Yes - create new names from the aggregate children and the old name at
    // the index.
    for (auto &suffix : getFlattenedNameSuffixes(it->second))
      newNames.push_back(StringAttr::get(
          op->getContext(), oldName + Twine(joinChar) + suffix));
  }
  return newNames;
}

template <typename ModTy>
static void updateModulePortNames(ModTy op, hw::ModuleType oldModType,
                                  char joinChar) {
  // Module arg and result port names may not be ordered. So we cannot reuse
  // updateNameAttribute. The arg and result order must be preserved.
  SmallVector<Attribute> newNames;
  SmallVector<hw::ModulePort> oldPorts(oldModType.getPorts().begin(),
                                       oldModType.getPorts().end());
  for (auto oldPort : oldPorts) {
    auto oldName = oldPort.name;
    if (isAggregateType(oldPort.type)) {
      for (auto &suffix : getFlattenedNameSuffixes(oldPort.type))
        newNames.push_back(StringAttr::get(
            op->getContext(), oldName.getValue() + Twine(joinChar) + suffix));
    } else
      newNames.push_back(oldName);
  }
  op.setAllPortNames(newNames);
}

static llvm::SmallVector<Location>
updateLocAttribute(DenseMap<unsigned, Type> &aggregateMap,
                   SmallVectorImpl<Location> &oldLocs) {
  llvm::SmallVector<Location> newLocs;
  for (auto [i, oldLoc] : llvm::enumerate(oldLocs)) {
    // Was this arg/res index an aggregate?
    auto it = aggregateMap.find(i);
    if (it == aggregateMap.end()) {
      // No, keep old name.
      newLocs.push_back(oldLoc);
      continue;
    }

    for (size_t i = 0, e = getFlattenedSize(it->second); i < e; ++i)
      newLocs.push_back(oldLoc);
  }
  return newLocs;
}

/// The conversion framework seems to throw away block argument locations.  We
/// use this function to copy the location from the original argument to the
/// set of flattened arguments.
static void
updateBlockLocations(hw::HWModuleLike op,
                     DenseMap<unsigned, Type> &aggregateMap) {
  auto locs = op.getInputLocs();
  if (locs.empty() || op.getModuleBody().empty())
    return;
  for (auto [arg, loc] : llvm::zip(op.getBodyBlock()->getArguments(), locs))
    arg.setLoc(loc);
}

static void setIOInfo(hw::HWModuleLike op, IOInfo &ioInfo) {
  ioInfo.argTypes = op.getInputTypes();
  ioInfo.resTypes = op.getOutputTypes();
  for (auto [i, arg] : llvm::enumerate(ioInfo.argTypes)) {
    if (isAggregateType(arg))
      ioInfo.argAggregates[i] = hw::getCanonicalType(arg);
  }
  for (auto [i, res] : llvm::enumerate(ioInfo.resTypes)) {
    if (isAggregateType(res))
      ioInfo.resAggregates[i] = hw::getCanonicalType(res);
  }
}

template <typename T>
static DenseMap<Operation *, IOInfo> populateIOInfoMap(mlir::ModuleOp module) {
  DenseMap<Operation *, IOInfo> ioInfoMap;
  for (auto op : module.getOps<T>()) {
    IOInfo ioInfo;
    setIOInfo(op, ioInfo);
    ioInfoMap[op] = ioInfo;
  }
  return ioInfoMap;
}

template <typename T>
static LogicalResult flattenOpsOfType(ModuleOp module, bool recursive,
                                      StringSet<> &externModules,
                                      char joinChar) {
  auto *ctx = module.getContext();
  FlattenIOTypeConverter typeConverter;

  // Recursively (in case of nested structs) lower the module. We do this one
  // conversion at a time to allow for updating the arg/res names of the
  // module in between flattening each level of structs.
  while (hasUnconvertedOps<T>(module)) {
    ConversionTarget target(*ctx);
    RewritePatternSet patterns(ctx);
    target.addLegalDialect<hw::HWDialect>();

    // Record any struct types at the module signature. This will be used
    // post-conversion to update the argument and result names.
    auto ioInfoMap = populateIOInfoMap<T>(module);

    // Record the instances that were converted. We keep these around since we
    // need to update their arg/res attribute names after the modules themselves
    // have been updated.
    llvm::DenseSet<hw::InstanceOp> convertedInstances;

    // Argument conversion for output ops. Similarly to the signature
    // conversion, legality is based on the op having been visited once, due to
    // the possibility of nested structs.
    DenseSet<Operation *> opVisited;
    patterns.add<OutputOpConversion>(typeConverter, ctx, &opVisited);

    patterns.add<InstanceOpConversion>(typeConverter, ctx, &convertedInstances,
                                       &externModules);
    target.addDynamicallyLegalOp<hw::OutputOp>(
        [&](auto op) { return opVisited.contains(op->getParentOp()); });
    target.addDynamicallyLegalOp<hw::InstanceOp>([&](hw::InstanceOp op) {
      auto refName = op.getReferencedModuleName();
      if (externModules.contains(refName))
        return true;
      // Instances already rewritten by this pass in the current round are
      // considered legal. Their operands and results may still contain
      // nested aggregates; those are flattened by the next round of the
      // enclosing conversion loop.
      if (convertedInstances.contains(op))
        return true;
      return llvm::none_of(op->getOperands(), [](auto operand) {
               return isAggregateType(operand.getType());
             }) &&
             llvm::none_of(op->getResultTypes(), [](auto result) {
               return isAggregateType(result);
             });
    });

    DenseMap<Operation *, ArrayAttr> oldArgNames, oldResNames;
    DenseMap<Operation *, SmallVector<Location>> oldArgLocs, oldResLocs;
    DenseMap<Operation *, hw::ModuleType> oldModTypes;

    for (auto op : module.getOps<T>()) {
      oldModTypes[op] = op.getHWModuleType();
      oldArgNames[op] = ArrayAttr::get(module.getContext(), op.getInputNames());
      oldResNames[op] =
          ArrayAttr::get(module.getContext(), op.getOutputNames());
      oldArgLocs[op] = op.getInputLocs();
      oldResLocs[op] = op.getOutputLocs();
    }

    // Signature conversion and legalization patterns.
    addSignatureConversion<T>(ioInfoMap, target, patterns, typeConverter);

    if (failed(applyPartialConversion(module, target, std::move(patterns))))
      return failure();

    // Update the arg/res names of the module.
    for (auto op : module.getOps<T>()) {
      auto ioInfo = ioInfoMap[op];
      updateModulePortNames(op, oldModTypes[op], joinChar);
      auto newArgLocs = updateLocAttribute(ioInfo.argAggregates, oldArgLocs[op]);
      auto newResLocs = updateLocAttribute(ioInfo.resAggregates, oldResLocs[op]);
      newArgLocs.append(newResLocs.begin(), newResLocs.end());
      op.setAllPortLocs(newArgLocs);
      updateBlockLocations(op, ioInfo.argAggregates);
    }

    // Update the arg/res names of all instances to match their referenced
    // modules. This has to be done for every instance, not just the ones
    // converted in this round: an instance flattened in an earlier round no
    // longer gets converted, but its referenced module may keep flattening
    // further levels, which would otherwise leave the instance names stale.
    module->walk([&](hw::InstanceOp instanceOp) {
      if (externModules.contains(instanceOp.getReferencedModuleName()))
        return;
      auto targetModule =
          cast<hw::HWModuleLike>(SymbolTable::lookupNearestSymbolFrom(
              instanceOp, instanceOp.getReferencedModuleNameAttr()));

      auto ioInfoIt = ioInfoMap.find(targetModule);
      if (ioInfoIt == ioInfoMap.end())
        return;
      auto &ioInfo = ioInfoIt->second;

      instanceOp.setInputNames(ArrayAttr::get(
          instanceOp.getContext(),
          updateNameAttribute(
              instanceOp, "argNames", ioInfo.argAggregates,
              oldArgNames[targetModule].template getAsValueRange<StringAttr>(),
              joinChar)));
      instanceOp.setOutputNames(ArrayAttr::get(
          instanceOp.getContext(),
          updateNameAttribute(
              instanceOp, "resultNames", ioInfo.resAggregates,
              oldResNames[targetModule].template getAsValueRange<StringAttr>(),
              joinChar)));
    });

    // Break if we've only lowering a single level of structs.
    if (!recursive)
      break;
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Pass driver
//===----------------------------------------------------------------------===//

template <typename... TOps>
static bool flattenIO(ModuleOp module, bool recursive,
                      StringSet<> &externModules, char joinChar) {
  return (failed(flattenOpsOfType<TOps>(module, recursive, externModules,
                                        joinChar)) ||
          ...);
}

namespace {

class FlattenIOPass : public circt::hw::impl::FlattenIOBase<FlattenIOPass> {
  using Base::Base;

public:
  void runOnOperation() override {
    ModuleOp module = getOperation();
    flattenArraysEnabled = flattenArrays;
    if (!flattenExtern) {
      // Record the extern modules, do not flatten them.
      for (auto m : module.getOps<hw::HWModuleExternOp>())
        externModules.insert(m.getModuleName());
      if (flattenIO<hw::HWModuleOp, hw::HWModuleGeneratedOp>(
              module, recursive, externModules, joinChar))
        signalPassFailure();
      return;
    }

    if (flattenIO<hw::HWModuleOp, hw::HWModuleExternOp,
                  hw::HWModuleGeneratedOp>(module, recursive, externModules,
                                           joinChar))
      signalPassFailure();
  };

private:
  StringSet<> externModules;
};
} // namespace
