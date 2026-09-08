//===- RegOfVecToMem.cpp - Convert Register Arrays to Memories -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This transformation pass converts register arrays that follow memory access
// patterns to seq.firmem operations.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqPasses.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"

#include <type_traits>

#define DEBUG_TYPE "reg-of-vec-to-mem"

using namespace circt;
using namespace seq;
using namespace hw;

namespace circt {
namespace seq {
#define GEN_PASS_DEF_REGOFVECTOMEM
#include "circt/Dialect/Seq/SeqPasses.h.inc"
} // namespace seq
} // namespace circt

namespace {

struct MemoryPattern {
  Operation *memReg = nullptr;   // The register array representing memory
  Value memValue;                // Current array value
  Type memType;                  // Array type
  FirRegOp outputReg;            // Optional output register
  Value clock;                   // Clock signal
  Value readAddr;                // Read address
  Value writeAddr;               // Write address
  Value writeData;               // Write data
  Value writeEnable;             // Write enable
  Value readEnable;              // Read enable (optional)
  SmallVector<comb::MuxOp> writeMuxes; // Mux tree selecting old/new state
  SmallVector<std::pair<Value, bool>> writeGuards; // condition and polarity
  comb::MuxOp readMux;           // Mux for read data
  hw::ArrayGetOp readAccess;     // Array read operation
  hw::ArrayInjectOp writeAccess; // Array write operation
};

class RegOfVecToMemPass : public impl::RegOfVecToMemBase<RegOfVecToMemPass> {
public:
  void runOnOperation() override;

private:
  template <typename RegOp>
  bool analyzeMemoryPattern(RegOp reg, MemoryPattern &pattern);
  bool createFirMemory(MemoryPattern &pattern);
  bool isArrayType(Type type);
  std::optional<std::pair<uint64_t, uint64_t>> getArrayDimensions(Type type);

  SmallVector<Operation *> opsToErase;
};

} // end anonymous namespace

bool RegOfVecToMemPass::isArrayType(Type type) {
  return isa<hw::ArrayType, hw::UnpackedArrayType>(type);
}

std::optional<std::pair<uint64_t, uint64_t>>
RegOfVecToMemPass::getArrayDimensions(Type type) {
  if (auto arrayType = dyn_cast<hw::ArrayType>(type)) {
    auto elemType = arrayType.getElementType();
    if (auto intType = dyn_cast<IntegerType>(elemType)) {
      return std::make_pair(arrayType.getNumElements(), intType.getWidth());
    }
  }
  return std::nullopt;
}

static bool isHoldValue(Value value, Value current,
                        SmallVectorImpl<comb::MuxOp> *muxes = nullptr) {
  if (value == current)
    return true;
  auto mux = value.getDefiningOp<comb::MuxOp>();
  if (!mux || !isHoldValue(mux.getTrueValue(), current, muxes) ||
      !isHoldValue(mux.getFalseValue(), current, muxes))
    return false;
  if (muxes)
    muxes->push_back(mux);
  return true;
}

static bool dependsOn(Value value, Operation *target, Value stopValue,
                      llvm::SmallPtrSetImpl<Operation *> &visited) {
  if (value == stopValue)
    return false;
  Operation *operation = value.getDefiningOp();
  if (!operation)
    return false;
  if (operation == target)
    return true;
  if (!visited.insert(operation).second)
    return false;
  return llvm::any_of(operation->getOperands(),
                      [&](Value operand) {
                        return dependsOn(operand, target, stopValue, visited);
                      });
}

template <typename RegOp>
bool RegOfVecToMemPass::analyzeMemoryPattern(RegOp reg,
                                             MemoryPattern &pattern) {
  LLVM_DEBUG(llvm::dbgs() << "Analyzing register: " << reg << "\n");

  // Check if register has array type
  if (!isArrayType(reg.getType()))
    return false;

  ArrayGetOp readAccess;
  ArrayInjectOp writeAccess;
  for (auto *user : reg.getResult().getUsers()) {
    LLVM_DEBUG(llvm::dbgs() << "  Register user: " << *user << "\n");
    if (auto arrayGet = dyn_cast<hw::ArrayGetOp>(user); !readAccess && arrayGet)
      readAccess = arrayGet;
    else if (auto arrayInject = dyn_cast<hw::ArrayInjectOp>(user);
             !writeAccess && arrayInject)
      writeAccess = arrayInject;
    else if (isa<comb::MuxOp>(user))
      continue;
    else
      return false;
  }
  if (!readAccess || !writeAccess)
    return false;

  pattern.memReg = reg.getOperation();
  pattern.memValue = reg.getResult();
  pattern.memType = reg.getType();
  pattern.clock = reg.getClk();

  // Find the mux that drives this register
  Value nextValue;
  if constexpr (std::is_same_v<RegOp, FirRegOp>)
    nextValue = reg.getNext();
  else
    nextValue = reg.getInput();
  auto mux = nextValue.getDefiningOp<comb::MuxOp>();
  if (!mux)
    return false;

  LLVM_DEBUG(llvm::dbgs() << "  Found driving mux: " << mux << "\n");
  // Follow a nested mux tree from next-state to the single indexed write.
  // Reset/enable lowering often creates several muxes whose other branch is
  // just a (possibly redundant) hold expression.
  Value cursor = nextValue;
  auto arrayInject = writeAccess;
  while (cursor != arrayInject.getResult()) {
    auto updateMux = cursor.getDefiningOp<comb::MuxOp>();
    if (!updateMux)
      return false;
    pattern.writeMuxes.push_back(updateMux);
    llvm::SmallPtrSet<Operation *, 16> trueVisited;
    llvm::SmallPtrSet<Operation *, 16> falseVisited;
    bool writeOnTrue = dependsOn(updateMux.getTrueValue(), arrayInject,
                                 reg.getResult(), trueVisited);
    bool writeOnFalse = dependsOn(updateMux.getFalseValue(), arrayInject,
                                  reg.getResult(), falseVisited);
    if (writeOnTrue == writeOnFalse)
      return false;
    Value hold = writeOnTrue ? updateMux.getFalseValue()
                             : updateMux.getTrueValue();
    if (!isHoldValue(hold, reg.getResult(), &pattern.writeMuxes))
      return false;
    pattern.writeGuards.push_back({updateMux.getCond(), writeOnTrue});
    cursor = writeOnTrue ? updateMux.getTrueValue()
                         : updateMux.getFalseValue();
  }
  if (arrayInject.getInput() != reg.getResult())
    return false;

  LLVM_DEBUG(llvm::dbgs() << "  Found array_inject: " << arrayInject << "\n");
  pattern.writeAccess = arrayInject;
  pattern.writeAddr = arrayInject.getIndex();
  pattern.writeData = arrayInject.getElement();
  pattern.writeEnable = {};

  // Look for read pattern - find array_get users
  auto arrayGet = readAccess;
  LLVM_DEBUG(llvm::dbgs() << "  Found array_get: " << arrayGet << "\n");
  pattern.readAccess = arrayGet;
  pattern.readAddr = arrayGet.getIndex();

  // Check if read goes through output register
  for (auto *readUser : arrayGet.getResult().getUsers()) {
    if (auto outputReg = dyn_cast<FirRegOp>(readUser)) {
      if (outputReg.getClk() == pattern.clock) {
        LLVM_DEBUG(llvm::dbgs()
                   << "  Found output register: " << outputReg << "\n");
        pattern.outputReg = outputReg;
        break;
      }
    }
  }

  bool success = pattern.readAccess != nullptr;
  LLVM_DEBUG(llvm::dbgs() << "  Pattern analysis "
                          << (success ? "succeeded" : "failed") << "\n");
  return success;
}

bool RegOfVecToMemPass::createFirMemory(MemoryPattern &pattern) {
  LLVM_DEBUG(llvm::dbgs() << "Creating FirMemory for pattern\n");

  auto dims = getArrayDimensions(pattern.memType);
  if (!dims)
    return false;

  uint64_t depth = dims->first;
  uint64_t width = dims->second;

  LLVM_DEBUG(llvm::dbgs() << "  Memory dimensions: " << depth << " x " << width
                          << "\n");

  ImplicitLocOpBuilder builder(pattern.memReg->getLoc(), pattern.memReg);

  // Create FirMem
  auto memType =
      FirMemType::get(builder.getContext(), depth, width, /*maskWidth=*/1);
  auto firMem = seq::FirMemOp::create(
      builder, memType, /*readLatency=*/0, /*writeLatency=*/1,
      /*readUnderWrite=*/seq::RUW::Undefined,
      /*writeUnderWrite=*/seq::WUW::Undefined,
      /*name=*/builder.getStringAttr("mem"), /*innerSym=*/hw::InnerSymAttr{},
      /*init=*/seq::FirMemInitAttr{}, /*prefix=*/StringAttr{},
      /*outputFile=*/Attribute{});

  // FIRRTL currently uses a 1-bit address for a single element memory,
  // however HW arrays use 0-bit addresses. To bridge this gap, create a 1-bit
  // address equal to 0 if our address is 0-bit.
  auto fixZeroWidthAddr = [&](Value addr) -> Value {
    if (addr.getType().getIntOrFloatBitWidth() == 0) {
      return hw::ConstantOp::create(builder,
                                    mlir::IntegerType::get(&getContext(), 1), 0)
          .getResult();
    }
    return addr;
  };

  // Create read port
  auto readAddr = fixZeroWidthAddr(pattern.readAddr);
  Value readData = FirMemReadOp::create(
      builder, firMem, readAddr, pattern.clock,
      /*enable=*/hw::ConstantOp::create(builder, builder.getI1Type(), 1));

  LLVM_DEBUG(llvm::dbgs() << "  Created read port\n"
                          << firMem << "\n " << readData);

  Value mask;
  // Create write port
  auto writeAddr = fixZeroWidthAddr(pattern.writeAddr);
  Value trueValue = hw::ConstantOp::create(builder, builder.getI1Type(), 1);
  Value writeEnable;
  for (auto [condition, positive] : pattern.writeGuards) {
    Value guard = condition;
    if (!positive)
      guard = comb::XorOp::create(builder, pattern.memReg->getLoc(), guard,
                                  trueValue);
    writeEnable = writeEnable
                      ? comb::AndOp::create(builder, pattern.memReg->getLoc(),
                                            writeEnable, guard, true)
                      : guard;
  }
  if (!writeEnable)
    writeEnable = trueValue;
  FirMemWriteOp::create(builder, firMem, writeAddr, pattern.clock,
                        writeEnable, pattern.writeData, mask);

  LLVM_DEBUG(llvm::dbgs() << "  Created write port\n");

  // Replace read access
  if (pattern.outputReg)
    // If there's an output register, replace its input
    pattern.outputReg.getNext().replaceAllUsesWith(readData);
  else
    // Replace direct read access
    pattern.readAccess.getResult().replaceAllUsesWith(readData);

  // Mark old operations for removal
  opsToErase.push_back(pattern.memReg);
  if (pattern.readAccess)
    opsToErase.push_back(pattern.readAccess);
  if (pattern.writeAccess)
    opsToErase.push_back(pattern.writeAccess);
  llvm::SmallPtrSet<Operation *, 8> seenMuxes;
  for (comb::MuxOp mux : pattern.writeMuxes)
    if (seenMuxes.insert(mux).second)
      opsToErase.push_back(mux);

  return true;
}

void RegOfVecToMemPass::runOnOperation() {
  auto module = getOperation();

  SmallVector<Operation *> arrayRegs;

  // Both FIRRTL and Core register forms can represent an inferred memory.
  module.walk([&](FirRegOp reg) {
    if (isArrayType(reg.getType()))
      arrayRegs.push_back(reg);
  });
  module.walk([&](CompRegOp reg) {
    if (isArrayType(reg.getType()))
      arrayRegs.push_back(reg);
  });

  // Analyze each array register for memory patterns
  for (Operation *reg : arrayRegs) {
    MemoryPattern pattern;
    bool matched = TypeSwitch<Operation *, bool>(reg)
                       .Case<FirRegOp, CompRegOp>([&](auto typedReg) {
                         return analyzeMemoryPattern(typedReg, pattern);
                       })
                       .Default(false);
    if (matched)
      createFirMemory(pattern);
  }

  // Erase all marked operations
  for (auto *op : opsToErase) {
    LLVM_DEBUG(llvm::dbgs()
               << "Erasing operation: " << *op << " number of uses:"
               << "\n");
    op->dropAllUses();
    op->erase();
  }
  opsToErase.clear();
}
