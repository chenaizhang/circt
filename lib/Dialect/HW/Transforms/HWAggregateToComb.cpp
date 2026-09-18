//===- HWAggregateToComb.cpp - HW aggregate to comb -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/HW/HWPasses.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallBitVector.h"

namespace circt {
namespace hw {
#define GEN_PASS_DEF_HWAGGREGATETOCOMB
#include "circt/Dialect/HW/Passes.h.inc"
} // namespace hw
} // namespace circt

using namespace mlir;
using namespace circt;

namespace {

static Attribute getZeroAttribute(Type type, Builder &builder) {
  if (auto intType = hw::type_dyn_cast<IntegerType>(type))
    return builder.getIntegerAttr(intType, 0);
  if (auto arrayType = hw::type_dyn_cast<hw::ArrayType>(type)) {
    Attribute element = getZeroAttribute(arrayType.getElementType(), builder);
    return builder.getArrayAttr(
        SmallVector<Attribute>(arrayType.getNumElements(), element));
  }
  if (auto structType = hw::type_dyn_cast<hw::StructType>(type)) {
    SmallVector<Attribute> fields;
    fields.reserve(structType.getElements().size());
    for (auto field : structType.getElements())
      fields.push_back(getZeroAttribute(field.type, builder));
    return builder.getArrayAttr(fields);
  }
  return {};
}

/// Track which output bits of an aggregate expression still depend on a
/// selected graph-cycle root. This is deliberately bit-accurate: generated
/// code often updates a nested array by extracting one inner array, changing
/// one element, and injecting it back into the outer array. Merely observing
/// that the outer slot was injected would incorrectly classify that partial
/// update as a complete overwrite.
class AggregateRootDependencyAnalysis {
public:
  explicit AggregateRootDependencyAnalysis(Value root) : root(root) {}

  FailureOr<llvm::SmallBitVector> analyzeRoot() {
    return analyze(root, /*expandRoot=*/true);
  }

  SmallVector<OpOperand *> &getBackedges() { return backedges; }

private:
  FailureOr<llvm::SmallBitVector> analyzeOperand(OpOperand &operand) {
    if (operand.get() == root ||
        (visiting.contains(operand.get()) &&
         operand.get().getType() == root.getType())) {
      backedges.push_back(&operand);
      int64_t width = hw::getBitWidth(operand.get().getType());
      if (width < 0)
        return failure();
      return llvm::SmallBitVector(width, true);
    }
    return analyze(operand.get(), /*expandRoot=*/false);
  }

  FailureOr<llvm::SmallBitVector> analyze(Value value, bool expandRoot) {
    int64_t width = hw::getBitWidth(value.getType());
    if (width < 0)
      return failure();
    if (value == root && !expandRoot)
      return llvm::SmallBitVector(width, true);
    if (!expandRoot)
      if (auto found = memo.find(value); found != memo.end())
        return found->second;

    Operation *def = value.getDefiningOp();
    if (!def)
      return llvm::SmallBitVector(width);
    if (!visiting.insert(value).second)
      return failure();

    auto finish = [&](FailureOr<llvm::SmallBitVector> result) {
      visiting.erase(value);
      if (!expandRoot && succeeded(result))
        memo.try_emplace(value, *result);
      return result;
    };
    auto allDependent = [&]() {
      return finish(llvm::SmallBitVector(width, true));
    };

    if (isa<hw::ConstantOp, hw::AggregateConstantOp>(def))
      return finish(llvm::SmallBitVector(width));

    // A register output is a state boundary. Its next-state and reset inputs
    // may be printed later and may themselves depend on this combinational
    // network, but those dependencies are not visible at the register output
    // during the current evaluation.
    if (isa<seq::CompRegOp, seq::CompRegClockEnabledOp, seq::FirRegOp>(def))
      return finish(llvm::SmallBitVector(width));

    if (auto inject = dyn_cast<hw::ArrayInjectOp>(def)) {
      APInt index;
      auto arrayType = hw::type_dyn_cast<hw::ArrayType>(value.getType());
      if (!arrayType ||
          !matchPattern(inject.getIndex(), m_ConstantInt(&index)) ||
          !index.isSingleWord() ||
          index.getZExtValue() >= arrayType.getNumElements())
        return finish(failure());
      auto base = analyzeOperand(inject.getInputMutable());
      auto element = analyzeOperand(inject.getElementMutable());
      if (failed(base) || failed(element))
        return finish(failure());
      int64_t elementWidth = hw::getBitWidth(arrayType.getElementType());
      if (elementWidth < 0 || element->size() != size_t(elementWidth))
        return finish(failure());
      size_t offset = index.getZExtValue() * elementWidth;
      for (size_t i = 0; i < size_t(elementWidth); ++i)
        (*base)[offset + i] = (*element)[i];
      return finish(std::move(base));
    }

    if (auto get = dyn_cast<hw::ArrayGetOp>(def)) {
      APInt index;
      auto arrayType = hw::type_dyn_cast<hw::ArrayType>(get.getInput().getType());
      if (!arrayType)
        return finish(failure());
      auto input = analyzeOperand(get.getInputMutable());
      if (failed(input) ||
          input->size() != arrayType.getNumElements() * size_t(width))
        return finish(failure());
      if (matchPattern(get.getIndex(), m_ConstantInt(&index)) &&
          index.isSingleWord() &&
          index.getZExtValue() < arrayType.getNumElements()) {
        size_t offset = index.getZExtValue() * width;
        llvm::SmallBitVector result(width);
        for (size_t i = 0; i < size_t(width); ++i)
          result[i] = (*input)[offset + i];
        return finish(std::move(result));
      }

      auto indexDependency = analyzeOperand(get->getOpOperand(1));
      if (failed(indexDependency))
        return finish(failure());
      if (indexDependency->any())
        return allDependent();

      // For an index independent of the root, an output bit may come from the
      // corresponding bit of any array element. Union those dependencies. In
      // particular, dynamically indexing a root-independent constant array
      // remains root-independent.
      llvm::SmallBitVector result(width);
      for (size_t element = 0; element < arrayType.getNumElements(); ++element)
        for (size_t i = 0; i < size_t(width); ++i)
          result[i] = result[i] || (*input)[element * width + i];
      return finish(std::move(result));
    }

    if (auto slice = dyn_cast<hw::ArraySliceOp>(def)) {
      APInt lowIndex;
      auto inputType =
          hw::type_dyn_cast<hw::ArrayType>(slice.getInput().getType());
      if (!inputType ||
          !matchPattern(slice.getLowIndex(), m_ConstantInt(&lowIndex)) ||
          !lowIndex.isSingleWord())
        return allDependent();
      int64_t elementWidth = hw::getBitWidth(inputType.getElementType());
      uint64_t offset = lowIndex.getZExtValue() * elementWidth;
      auto input = analyzeOperand(slice->getOpOperand(0));
      if (failed(input) || elementWidth < 0 || offset + width > input->size())
        return finish(failure());
      llvm::SmallBitVector result(width);
      for (size_t i = 0; i < size_t(width); ++i)
        result[i] = (*input)[offset + i];
      return finish(std::move(result));
    }

    if (auto extract = dyn_cast<hw::StructExtractOp>(def)) {
      auto structType =
          hw::type_dyn_cast<hw::StructType>(extract.getInput().getType());
      if (!structType)
        return finish(failure());
      int64_t totalWidth = hw::getBitWidth(structType);
      int64_t consumedWidth = 0;
      for (size_t i = 0; i < extract.getFieldIndex(); ++i) {
        int64_t fieldWidth =
            hw::getBitWidth(structType.getElements()[i].type);
        if (fieldWidth < 0)
          return finish(failure());
        consumedWidth += fieldWidth;
      }
      int64_t offset = totalWidth - consumedWidth - width;
      auto input = analyzeOperand(extract->getOpOperand(0));
      if (failed(input) || offset < 0 || offset + width > int64_t(input->size()))
        return finish(failure());
      llvm::SmallBitVector result(width);
      for (size_t i = 0; i < size_t(width); ++i)
        result[i] = (*input)[offset + i];
      return finish(std::move(result));
    }

    if (auto inject = dyn_cast<hw::StructInjectOp>(def)) {
      auto structType = hw::type_dyn_cast<hw::StructType>(value.getType());
      if (!structType)
        return finish(failure());
      int64_t consumedWidth = 0;
      for (size_t i = 0; i < inject.getFieldIndex(); ++i) {
        int64_t precedingWidth =
            hw::getBitWidth(structType.getElements()[i].type);
        if (precedingWidth < 0)
          return finish(failure());
        consumedWidth += precedingWidth;
      }
      int64_t fieldWidth =
          hw::getBitWidth(structType.getElements()[inject.getFieldIndex()].type);
      if (fieldWidth < 0)
        return finish(failure());
      int64_t offset = width - consumedWidth - fieldWidth;
      auto base = analyzeOperand(inject->getOpOperand(0));
      auto field = analyzeOperand(inject->getOpOperand(1));
      if (failed(base) || failed(field) || offset < 0 ||
          field->size() != size_t(fieldWidth))
        return finish(failure());
      for (size_t i = 0; i < size_t(fieldWidth); ++i)
        (*base)[offset + i] = (*field)[i];
      return finish(std::move(base));
    }

    if (auto mux = dyn_cast<comb::MuxOp>(def)) {
      auto condition = analyzeOperand(mux->getOpOperand(0));
      auto trueResult = analyzeOperand(mux->getOpOperand(1));
      auto falseResult = analyzeOperand(mux->getOpOperand(2));
      if (failed(condition) || failed(trueResult) || failed(falseResult))
        return finish(failure());
      if (condition->any())
        return allDependent();
      *trueResult |= *falseResult;
      return finish(std::move(trueResult));
    }

    if (auto bitcast = dyn_cast<hw::BitcastOp>(def)) {
      auto input = analyzeOperand(bitcast->getOpOperand(0));
      if (failed(input) || input->size() != size_t(width))
        return finish(failure());
      return finish(std::move(input));
    }

    if (isa<hw::ArrayCreateOp, hw::ArrayConcatOp, hw::StructCreateOp>(def)) {
      llvm::SmallBitVector result(width);
      size_t offset = width;
      for (OpOperand &operand : def->getOpOperands()) {
        auto element = analyzeOperand(operand);
        if (failed(element) || element->size() > offset)
          return finish(failure());
        offset -= element->size();
        for (size_t i = 0; i < element->size(); ++i)
          result[offset + i] = (*element)[i];
      }
      return finish(std::move(result));
    }

    // Conservatively handle the remaining scalar and aggregate operations.
    // If none of their operands depends on the root, neither does the result;
    // otherwise assume every result bit may depend on it.
    for (OpOperand &operand : def->getOpOperands()) {
      auto dependency = analyzeOperand(operand);
      if (failed(dependency))
        return finish(failure());
      if (dependency->any())
        return allDependent();
    }
    return finish(llvm::SmallBitVector(width));
  }

  Value root;
  SmallPtrSet<Value, 16> visiting;
  DenseMap<Value, llvm::SmallBitVector> memo;
  SmallVector<OpOperand *> backedges;
};

/// Slang may lower a completely assigned combinational array temporary into
/// a graph-region feedback cycle. Each `hw.array_inject` keeps the untouched
/// elements from the eventual result even though one pass through the chain
/// overwrites every element. Prove that property across every mux branch and
/// replace only the feedback operands reached during the proof with a zero
/// base. Partial updates are deliberately left untouched since they can
/// represent a latch or a real combinational loop.
static void breakFullyOverwrittenArrayCycles(Operation *root) {
  bool changed;
  do {
    changed = false;
    root->walk([&](Operation *op) {
      if (op->getNumResults() != 1)
        return;
      Value result = op->getResult(0);
      auto arrayType = hw::type_dyn_cast<hw::ArrayType>(result.getType());
      if (!arrayType || arrayType.getNumElements() == 0)
        return;

      // Every cycle in a single-block data-flow graph contains at least one
      // backward edge. Select only the producer on that edge: its result is
      // used by an operation printed before the producer. Selecting consumers
      // merely because one operand is defined later includes most nodes in a
      // large update chain and repeatedly analyzes the same graph.
      bool participatesInForwardReference =
          llvm::any_of(result.getUses(), [&](OpOperand &use) {
            Operation *owner = use.getOwner();
            return owner->getBlock() == op->getBlock() &&
                   owner->isBeforeInBlock(op);
          });
      if (!participatesInForwardReference)
        return;

      if (!isa<hw::ArrayInjectOp, comb::MuxOp>(op))
        return;

      AggregateRootDependencyAnalysis analysis(result);
      auto dependency = analysis.analyzeRoot();
      auto &backedges = analysis.getBackedges();
      if (failed(dependency) || backedges.empty() || dependency->any())
        return;

      OpBuilder builder(op);
      Attribute zero = getZeroAttribute(arrayType, builder);
      if (!zero)
        return;
      Value zeroArray = hw::AggregateConstantOp::create(
          builder, op->getLoc(), arrayType, cast<ArrayAttr>(zero));
      for (OpOperand *backedge : backedges)
        backedge->set(zeroArray);
      changed = true;
    });
  } while (changed);
}

// Lower hw.array_create and hw.array_concat to comb.concat.
template <typename OpTy>
struct HWArrayCreateLikeOpConversion : OpConversionPattern<OpTy> {
  using OpConversionPattern<OpTy>::OpConversionPattern;
  using OpAdaptor = typename OpConversionPattern<OpTy>::OpAdaptor;
  LogicalResult
  matchAndRewrite(OpTy op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<comb::ConcatOp>(op, adaptor.getInputs());
    return success();
  }
};

struct HWAggregateConstantOpConversion
    : OpConversionPattern<hw::AggregateConstantOp> {
  using OpConversionPattern<hw::AggregateConstantOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(hw::AggregateConstantOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Lower to concat.
    APInt intVal;
    if (failed(hw::aggregateAttrToAPInt(op.getType(), adaptor.getFieldsAttr(),
                                        intVal)))
      return failure();
    rewriter.replaceOpWithNewOp<hw::ConstantOp>(op, intVal);
    return success();
  }
};

struct HWArrayGetOpConversion : OpConversionPattern<hw::ArrayGetOp> {
  using OpConversionPattern<hw::ArrayGetOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(hw::ArrayGetOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    SmallVector<Value> results;
    auto arrayType = cast<hw::ArrayType>(op.getInput().getType());
    auto elemType = arrayType.getElementType();
    auto numElements = arrayType.getNumElements();
    auto elemWidth = hw::getBitWidth(elemType);
    if (elemWidth < 0)
      return rewriter.notifyMatchFailure(op.getLoc(), "unknown element width");

    auto lowered = adaptor.getInput();
    auto index = adaptor.getIndex();
    APInt constantIndex;
    if (matchPattern(index, m_ConstantInt(&constantIndex))) {
      int64_t maxIndex = std::numeric_limits<int32_t>::max() / elemWidth;
      if (constantIndex.isSingleWord() &&
          constantIndex.getZExtValue() <= static_cast<uint64_t>(maxIndex)) {
        rewriter.replaceOpWithNewOp<comb::ExtractOp>(
            op, lowered, constantIndex.getZExtValue() * elemWidth, elemWidth);
        return success();
      }
    }

    for (size_t i = 0; i < numElements; ++i)
      results.push_back(rewriter.createOrFold<comb::ExtractOp>(
          op.getLoc(), lowered, i * elemWidth, elemWidth));

    SmallVector<Value> bits;
    comb::extractBits(rewriter, index, bits);
    auto result = comb::constructMuxTree(rewriter, op.getLoc(), bits, results,
                                         results.back());

    rewriter.replaceOp(op, result);
    return success();
  }
};

struct HWArrayInjectOpConversion : OpConversionPattern<hw::ArrayInjectOp> {
  using OpConversionPattern<hw::ArrayInjectOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(hw::ArrayInjectOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto arrayType = cast<hw::ArrayType>(op.getInput().getType());
    auto elemType = arrayType.getElementType();
    auto numElements = arrayType.getNumElements();
    auto elemWidth = hw::getBitWidth(elemType);
    if (elemWidth < 0)
      return rewriter.notifyMatchFailure(op.getLoc(), "unknown element width");

    Location loc = op.getLoc();

    // Extract all elements from the input array
    SmallVector<Value> originalElements;
    auto inputArray = adaptor.getInput();
    for (size_t i = 0; i < numElements; ++i) {
      originalElements.push_back(rewriter.createOrFold<comb::ExtractOp>(
          loc, inputArray, i * elemWidth, elemWidth));
    }

    // A constant index needs only one rebuilt array. The generic dynamic
    // lowering below constructs every possible row of an N x N array before
    // selecting one; doing that for the constant-index update chains emitted
    // by frontends causes a quadratic IR blow-up, which compounds badly for
    // nested arrays. Array element 0 occupies the LSBs, so concatenate the
    // rebuilt elements in reverse order.
    APInt constantIndex;
    if (matchPattern(adaptor.getIndex(), m_ConstantInt(&constantIndex)) &&
        constantIndex.isSingleWord() &&
        constantIndex.getZExtValue() < numElements) {
      uint64_t injectIndex = constantIndex.getZExtValue();
      SmallVector<Value> elements;
      elements.reserve(numElements);
      for (int64_t i = numElements - 1; i >= 0; --i)
        elements.push_back(i == int64_t(injectIndex) ? adaptor.getElement()
                                                     : originalElements[i]);
      rewriter.replaceOpWithNewOp<comb::ConcatOp>(op, elements);
      return success();
    }

    // Create 2D array: each row represents what the array would look like
    // if injection happened at that specific index
    SmallVector<Value> arrayRows;
    arrayRows.reserve(numElements);
    for (int injectIdx = numElements - 1; injectIdx >= 0; --injectIdx) {
      SmallVector<Value> rowElements;
      rowElements.reserve(numElements);

      // Build the row: array[n-1], array[n-2], ..., but replace element at
      // injectIdx with newVal
      for (int originalIdx = numElements - 1; originalIdx >= 0; --originalIdx) {
        if (originalIdx == injectIdx) {
          rowElements.push_back(adaptor.getElement());
        } else {
          rowElements.push_back(originalElements[originalIdx]);
        }
      }

      // Concatenate elements to form this row
      Value row = hw::ArrayCreateOp::create(rewriter, loc, rowElements);
      arrayRows.push_back(row);
    }

    // Create the 2D array by concatenating all rows
    // arrayRows[0] corresponds to injection at index 0
    // arrayRows[1] corresponds to injection at index 1, etc.
    Value array2D = hw::ArrayCreateOp::create(rewriter, loc, arrayRows);

    // Create array_get operation to select the row
    auto arrayGetOp =
        hw::ArrayGetOp::create(rewriter, loc, array2D, adaptor.getIndex());

    rewriter.replaceOp(op, arrayGetOp);
    return success();
  }
};

struct HWStructCreateOpConversion : OpConversionPattern<hw::StructCreateOp> {
  using OpConversionPattern<hw::StructCreateOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(hw::StructCreateOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Lower struct_create to comb.concat. The first field occupies the MSBs, so
    // we concatenate fields in order (comb.concat places first operand at MSB).
    rewriter.replaceOpWithNewOp<comb::ConcatOp>(op, adaptor.getInput());
    return success();
  }
};

struct HWStructExtractOpConversion : OpConversionPattern<hw::StructExtractOp> {
  using OpConversionPattern<hw::StructExtractOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(hw::StructExtractOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto structType = cast<hw::StructType>(op.getInput().getType());
    auto fieldIndex = op.getFieldIndex();
    auto elements = structType.getElements();

    int64_t totalBitWidth = hw::getBitWidth(structType);
    if (totalBitWidth < 0)
      return rewriter.notifyMatchFailure(op.getLoc(), "unknown struct width");

    // Compute the bit offset from the MSB by summing the widths of all
    // preceding fields. The first field occupies the MSBs.
    int64_t consumedBits = 0;
    for (size_t i = 0; i < fieldIndex; ++i) {
      int64_t fieldWidth = hw::getBitWidth(elements[i].type);
      assert(fieldWidth >= 0 &&
             "must be failed before if field width is unknown");
      consumedBits += fieldWidth;
    }

    int64_t fieldWidth = hw::getBitWidth(elements[fieldIndex].type);
    assert(fieldWidth >= 0 &&
           "must be failed before if field width is unknown");

    // Extract the field using comb.extract. Offset is from LSB.
    int64_t bitOffset = totalBitWidth - consumedBits - fieldWidth;
    rewriter.replaceOpWithNewOp<comb::ExtractOp>(op, adaptor.getInput(),
                                                 bitOffset, fieldWidth);
    return success();
  }
};

/// Lower a struct explode into one extract per field. The first field
/// occupies the MSBs, matching the struct_extract lowering.
struct HWStructExplodeOpConversion : OpConversionPattern<hw::StructExplodeOp> {
  using OpConversionPattern<hw::StructExplodeOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(hw::StructExplodeOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto structType = cast<hw::StructType>(op.getInput().getType());
    auto elements = structType.getElements();

    int64_t totalBitWidth = hw::getBitWidth(structType);
    if (totalBitWidth < 0)
      return rewriter.notifyMatchFailure(op.getLoc(), "unknown struct width");

    SmallVector<Value> fields;
    int64_t consumedBits = 0;
    for (auto element : elements) {
      int64_t fieldWidth = hw::getBitWidth(element.type);
      if (fieldWidth < 0)
        return rewriter.notifyMatchFailure(op.getLoc(), "unknown field width");
      int64_t bitOffset = totalBitWidth - consumedBits - fieldWidth;
      fields.push_back(rewriter.createOrFold<comb::ExtractOp>(
          op.getLoc(), adaptor.getInput(), bitOffset, fieldWidth));
      consumedBits += fieldWidth;
    }

    rewriter.replaceOp(op, fields);
    return success();
  }
};

/// Lower a struct inject by rebuilding the struct from its fields, with the
/// injected field substituted. The first field occupies the MSBs, matching
/// the struct_extract lowering.
struct HWStructInjectOpConversion : OpConversionPattern<hw::StructInjectOp> {
  using OpConversionPattern<hw::StructInjectOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(hw::StructInjectOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto structType = cast<hw::StructType>(op.getInput().getType());
    auto elements = structType.getElements();
    auto fieldIndex = op.getFieldIndex();

    int64_t totalBitWidth = hw::getBitWidth(structType);
    if (totalBitWidth < 0)
      return rewriter.notifyMatchFailure(op.getLoc(), "unknown struct width");

    SmallVector<Value> fields;
    int64_t consumedBits = 0;
    for (size_t i = 0; i < elements.size(); ++i) {
      int64_t fieldWidth = hw::getBitWidth(elements[i].type);
      if (fieldWidth < 0)
        return rewriter.notifyMatchFailure(op.getLoc(), "unknown field width");
      Value field;
      if (i == fieldIndex) {
        field = adaptor.getNewValue();
      } else {
        int64_t bitOffset = totalBitWidth - consumedBits - fieldWidth;
        field = rewriter.createOrFold<comb::ExtractOp>(
            op.getLoc(), adaptor.getInput(), bitOffset, fieldWidth);
      }
      fields.push_back(field);
      consumedBits += fieldWidth;
    }

    rewriter.replaceOpWithNewOp<comb::ConcatOp>(op, fields);
    return success();
  }
};

/// Lower an array slice into a concat of element extracts. Result element i
/// holds input element `lowIndex + i`; element 0 sits in the LSBs of the
/// flattened representation, so the concat is built MSB-first.
struct HWArraySliceOpConversion : OpConversionPattern<hw::ArraySliceOp> {
  using OpConversionPattern<hw::ArraySliceOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(hw::ArraySliceOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto arrayType = cast<hw::ArrayType>(op.getInput().getType());
    auto resultType = cast<hw::ArrayType>(op.getType());
    auto elemWidth = hw::getBitWidth(arrayType.getElementType());
    auto numElements = resultType.getNumElements();
    auto numInputElements = arrayType.getNumElements();
    if (elemWidth < 0)
      return rewriter.notifyMatchFailure(op.getLoc(), "unknown element width");

    Location loc = op.getLoc();
    Value lowered = adaptor.getInput();
    Value index = adaptor.getLowIndex();

    // Extract a single element word from the flattened input. With a constant
    // index this folds to a plain extract; otherwise a mux tree over all
    // input words selects the requested one.
    auto getWord = [&](Value idx) -> Value {
      APInt constantIndex;
      if (matchPattern(idx, m_ConstantInt(&constantIndex)) &&
          constantIndex.isSingleWord() &&
          constantIndex.getZExtValue() < numInputElements) {
        return rewriter.createOrFold<comb::ExtractOp>(
            loc, lowered, constantIndex.getZExtValue() * elemWidth, elemWidth);
      }
      SmallVector<Value> words;
      for (uint64_t w = 0; w < numInputElements; ++w)
        words.push_back(rewriter.createOrFold<comb::ExtractOp>(
            loc, lowered, w * elemWidth, elemWidth));
      SmallVector<Value> bits;
      comb::extractBits(rewriter, idx, bits);
      return comb::constructMuxTree(rewriter, loc, bits, words, words.back());
    };

    SmallVector<Value> fields;
    fields.reserve(numElements);
    auto idxWidth = index.getType().getIntOrFloatBitWidth();
    APInt constantBase;
    bool hasConstantBase = matchPattern(index, m_ConstantInt(&constantBase));
    for (int64_t i = numElements - 1; i >= 0; --i) {
      Value idx;
      if (hasConstantBase) {
        idx = rewriter.createOrFold<hw::ConstantOp>(
            loc, APInt(idxWidth, constantBase.getZExtValue() + i));
      } else {
        idx = comb::AddOp::create(
            rewriter, loc, index,
            rewriter.create<hw::ConstantOp>(loc, APInt(idxWidth, i)), false);
      }
      fields.push_back(getWord(idx));
    }

    rewriter.replaceOpWithNewOp<comb::ConcatOp>(op, fields);
    return success();
  }
};

struct MuxOpConversion : OpConversionPattern<comb::MuxOp> {
  using OpConversionPattern<comb::MuxOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(comb::MuxOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Re-create Mux with legalized types.
    rewriter.replaceOpWithNewOp<comb::MuxOp>(
        op, adaptor.getCond(), adaptor.getTrueValue(), adaptor.getFalseValue());
    return success();
  }
};

/// Enumerate the children of an aggregate type in flattened bit order:
/// struct field 0 occupies the MSBs; array element 0 sits in the LSBs. For
/// each child, the offset (from LSB) and width of its bits, plus a name
/// suffix, are reported.
static LogicalResult forEachAggregateField(
    Type type, const std::function<LogicalResult(int64_t, int64_t,
                                                 const std::string &)> &fn) {
  if (auto structType = dyn_cast<hw::StructType>(type)) {
    int64_t totalBitWidth = hw::getBitWidth(structType);
    int64_t consumedBits = 0;
    for (auto field : structType.getElements()) {
      int64_t fieldWidth = hw::getBitWidth(field.type);
      if (fieldWidth < 0)
        return failure();
      int64_t bitOffset = totalBitWidth - consumedBits - fieldWidth;
      if (failed(fn(bitOffset, fieldWidth, field.name.getValue().str())))
        return failure();
      consumedBits += fieldWidth;
    }
    return success();
  }
  if (auto arrayType = dyn_cast<hw::ArrayType>(type)) {
    int64_t elemWidth = hw::getBitWidth(arrayType.getElementType());
    if (elemWidth < 0)
      return failure();
    for (uint64_t i = 0; i < arrayType.getNumElements(); ++i) {
      if (failed(fn(i * elemWidth, elemWidth, std::to_string(i))))
        return failure();
    }
    return success();
  }
  return failure();
}

/// Split a Seq register holding an aggregate value into per-child scalar
/// registers. The register data is rebuilt with an aggregate create; the next
/// and reset values are decomposed with bit extracts matching the flattened
/// bit order of the aggregate.
template <typename RegTy>
struct AggregateRegisterConversion : OpConversionPattern<RegTy> {
  using OpConversionPattern<RegTy>::OpConversionPattern;
  using OpAdaptor = typename OpConversionPattern<RegTy>::OpAdaptor;

  LogicalResult
  matchAndRewrite(RegTy reg, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto aggType = reg.getType();
    if (!isa<hw::StructType, hw::ArrayType>(aggType))
      return rewriter.notifyMatchFailure(reg, "not an aggregate register");

    if constexpr (std::is_same_v<RegTy, seq::FirRegOp>) {
      if (reg.hasPresetValue())
        return rewriter.notifyMatchFailure(reg,
                                           "preset values are not supported");
    } else {
      // CompReg ops with a reset are handled by dedicated builders only for
      // the reset-less form here; DSC does not require them.
      if (reg.getReset())
        return rewriter.notifyMatchFailure(
            reg, "aggregate registers with reset are not supported for this "
                 "register kind");
    }

    Location loc = reg.getLoc();
    SmallVector<Value> children;
    SmallVector<std::string> suffixes;

    // Decompose next (and reset) values into the scalar children.
    auto getChild = [&](Value lowered, int64_t offset,
                        int64_t width) -> Value {
      return rewriter.createOrFold<comb::ExtractOp>(loc, lowered, offset,
                                                    width);
    };

    LogicalResult result = forEachAggregateField(
        aggType, [&](int64_t offset, int64_t width,
                     const std::string &suffix) -> LogicalResult {
      Value nextValue;
      if constexpr (std::is_same_v<RegTy, seq::FirRegOp>)
        nextValue = adaptor.getNext();
      else
        nextValue = adaptor.getInput();
      Value next = getChild(nextValue, offset, width);

      Value scalarReg;
      if constexpr (std::is_same_v<RegTy, seq::CompRegOp>) {
        scalarReg = rewriter.create<seq::CompRegOp>(loc, next, adaptor.getClk());
      } else if constexpr (std::is_same_v<RegTy,
                                          seq::CompRegClockEnabledOp>) {
        scalarReg = rewriter.create<seq::CompRegClockEnabledOp>(
            loc, next, adaptor.getClk(), adaptor.getClockEnable(),
            rewriter.getStringAttr(""));
      } else {
        // seq::FirRegOp
        StringAttr name =
            reg.getNameAttr() ? rewriter.getStringAttr(
                                    reg.getNameAttr().getValue() + "_" +
                                    suffix)
                              : rewriter.getStringAttr("");
        if (reg.getReset()) {
          Value resetValue =
              getChild(adaptor.getResetValue(), offset, width);
          scalarReg = rewriter.create<seq::FirRegOp>(
              loc, next, adaptor.getClk(), name, adaptor.getReset(),
              resetValue, hw::InnerSymAttr(), reg.getIsAsync(), Attribute());
        } else {
          scalarReg = rewriter.create<seq::FirRegOp>(
              loc, next, adaptor.getClk(), name, hw::InnerSymAttr(),
              Attribute());
        }
      }
      children.push_back(scalarReg);
      suffixes.push_back(suffix);
      return success();
    });
    if (failed(result))
      return failure();

    // Rebuild the aggregate value from the scalar registers.
    Value imploded;
    if (isa<hw::StructType>(aggType)) {
      imploded = hw::StructCreateOp::create(rewriter, loc, aggType, children);
    } else {
      SmallVector<Value> reversed(children.rbegin(), children.rend());
      imploded = hw::ArrayCreateOp::create(rewriter, loc, reversed);
    }
    rewriter.replaceOp(reg, imploded);
    return success();
  }
};

/// A type converter is needed to perform the in-flight materialization of
/// aggregate types to integer types.
class AggregateTypeConverter : public TypeConverter {
public:
  AggregateTypeConverter() {
    addConversion([](Type type) -> Type { return type; });
    addConversion([](hw::ArrayType t) -> Type {
      return IntegerType::get(t.getContext(), hw::getBitWidth(t));
    });
    addConversion([](hw::StructType t) -> Type {
      return IntegerType::get(t.getContext(), hw::getBitWidth(t));
    });
    addTargetMaterialization([](mlir::OpBuilder &builder, mlir::Type resultType,
                                mlir::ValueRange inputs,
                                mlir::Location loc) -> mlir::Value {
      if (inputs.size() != 1)
        return Value();

      return hw::BitcastOp::create(builder, loc, resultType, inputs[0])
          ->getResult(0);
    });

    addSourceMaterialization([](mlir::OpBuilder &builder, mlir::Type resultType,
                                mlir::ValueRange inputs,
                                mlir::Location loc) -> mlir::Value {
      if (inputs.size() != 1)
        return Value();

      return hw::BitcastOp::create(builder, loc, resultType, inputs[0])
          ->getResult(0);
    });
  }
};
} // namespace

static void populateHWAggregateToCombOpConversionPatterns(
    RewritePatternSet &patterns, AggregateTypeConverter &typeConverter) {
  patterns.add<
      HWArrayGetOpConversion, HWArrayCreateLikeOpConversion<hw::ArrayCreateOp>,
      HWArrayCreateLikeOpConversion<hw::ArrayConcatOp>,
      HWAggregateConstantOpConversion, HWArrayInjectOpConversion,
      HWArraySliceOpConversion, HWStructCreateOpConversion,
      HWStructExtractOpConversion, HWStructExplodeOpConversion,
      HWStructInjectOpConversion, AggregateRegisterConversion<seq::CompRegOp>,
      AggregateRegisterConversion<seq::CompRegClockEnabledOp>,
      AggregateRegisterConversion<seq::FirRegOp>, MuxOpConversion>(
      typeConverter, patterns.getContext());
}

namespace {
struct HWAggregateToCombPass
    : public hw::impl::HWAggregateToCombBase<HWAggregateToCombPass> {
  void runOnOperation() override;
  using HWAggregateToCombBase<HWAggregateToCombPass>::HWAggregateToCombBase;
};
} // namespace

void HWAggregateToCombPass::runOnOperation() {
  breakFullyOverwrittenArrayCycles(getOperation());

  ConversionTarget target(getContext());

  target.addIllegalOp<hw::ArrayGetOp, hw::ArrayCreateOp, hw::ArrayConcatOp,
                      hw::ArraySliceOp, hw::AggregateConstantOp,
                      hw::ArrayInjectOp, hw::StructCreateOp,
                      hw::StructExtractOp, hw::StructExplodeOp,
                      hw::StructInjectOp>();
  target.addDynamicallyLegalOp<comb::MuxOp>(
      [](comb::MuxOp op) { return hw::type_isa<IntegerType>(op.getType()); });
  target.addDynamicallyLegalOp<seq::CompRegOp, seq::CompRegClockEnabledOp,
                               seq::FirRegOp>([](Operation *op) {
    return !isa<hw::StructType, hw::ArrayType>(op->getResult(0).getType());
  });
  target.addLegalDialect<hw::HWDialect, comb::CombDialect>();

  RewritePatternSet patterns(&getContext());
  AggregateTypeConverter typeConverter;
  populateHWAggregateToCombOpConversionPatterns(patterns, typeConverter);

  if (failed(mlir::applyPartialConversion(getOperation(), target,
                                          std::move(patterns))))
    return signalPassFailure();
}
