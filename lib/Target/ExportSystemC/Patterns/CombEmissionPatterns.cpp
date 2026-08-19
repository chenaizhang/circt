//===- CombEmissionPatterns.cpp - Comb emission patterns -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CombEmissionPatterns.h"
#include "../EmissionPattern.h"
#include "../EmissionPrinter.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace circt;
using namespace circt::comb;
using namespace circt::ExportSystemC;

namespace {

static unsigned getWidth(Type type) {
  return cast<IntegerType>(type).getWidth();
}

static void emitFixedWidthType(EmissionPrinter &p, unsigned width,
                               bool isSigned = false) {
  if (width <= 64)
    p << (isSigned ? "sc_int<" : "sc_uint<") << width << ">";
  else if (width <= 512)
    p << (isSigned ? "sc_bigint<" : "sc_biguint<") << width << ">";
  else
    p << "sc_bv<" << width << ">";
}

static StringRef getOperator(Operation *op) {
  return TypeSwitch<Operation *, StringRef>(op)
      .Case<AddOp>([](auto) { return " + "; })
      .Case<SubOp>([](auto) { return " - "; })
      .Case<MulOp>([](auto) { return " * "; })
      .Case<DivUOp, DivSOp>([](auto) { return " / "; })
      .Case<ModUOp, ModSOp>([](auto) { return " % "; })
      .Case<ShlOp>([](auto) { return " << "; })
      .Case<ShrUOp, ShrSOp>([](auto) { return " >> "; })
      .Case<AndOp>([](auto) { return " & "; })
      .Case<OrOp>([](auto) { return " | "; })
      .Case<XorOp>([](auto) { return " ^ "; })
      .Default([](auto) { return ""; });
}

static Precedence getPrecedence(Operation *op) {
  return TypeSwitch<Operation *, Precedence>(op)
      .Case<AddOp>([](auto) { return Precedence::ADD; })
      .Case<SubOp>([](auto) { return Precedence::SUB; })
      .Case<MulOp>([](auto) { return Precedence::MUL; })
      .Case<DivUOp, DivSOp>([](auto) { return Precedence::DIV; })
      .Case<ModUOp, ModSOp>([](auto) { return Precedence::MOD; })
      .Case<ShlOp>([](auto) { return Precedence::SHL; })
      .Case<ShrUOp, ShrSOp>([](auto) { return Precedence::SHR; })
      .Case<AndOp>([](auto) { return Precedence::BITWISE_AND; })
      .Case<OrOp>([](auto) { return Precedence::BITWISE_OR; })
      .Case<XorOp>([](auto) { return Precedence::BITWISE_XOR; })
      .Default([](auto) { return Precedence::LIT; });
}

static bool isSignedOperation(Operation *op) {
  return isa<DivSOp, ModSOp, ShrSOp>(op);
}

static void emitOperand(Value value, Precedence precedence,
                        EmissionPrinter &p) {
  p.getInlinable(value).emitWithParensOnLowerPrecedence(precedence);
}

static void emitUnsignedCast(Value value, EmissionPrinter &p) {
  emitFixedWidthType(p, getWidth(value.getType()));
  p << "(";
  p.getInlinable(value).emit();
  p << ")";
}

template <typename Op>
struct VariadicExpressionEmitter : OpEmissionPattern<Op> {
  using OpEmissionPattern<Op>::OpEmissionPattern;

  MatchResult matchInlinable(Value value) override {
    auto op = value.getDefiningOp<Op>();
    if (!op || getWidth(value.getType()) > 512)
      return {};
    return Precedence::FUNCTIONAL_CAST;
  }

  void emitInlined(Value value, EmissionPrinter &p) override {
    auto op = value.getDefiningOp<Op>();
    emitFixedWidthType(p, getWidth(value.getType()), isSignedOperation(op));
    p << "(";
    llvm::interleave(
        op.getInputs(), p,
        [&](Value operand) { emitOperand(operand, getPrecedence(op), p); },
        getOperator(op));
    p << ")";
  }
};

template <typename Op>
struct BinaryExpressionEmitter : OpEmissionPattern<Op> {
  using OpEmissionPattern<Op>::OpEmissionPattern;

  MatchResult matchInlinable(Value value) override {
    auto op = value.getDefiningOp<Op>();
    if (!op || getWidth(value.getType()) > 512)
      return {};
    return Precedence::FUNCTIONAL_CAST;
  }

  void emitInlined(Value value, EmissionPrinter &p) override {
    auto op = value.getDefiningOp<Op>();
    emitFixedWidthType(p, getWidth(value.getType()), isSignedOperation(op));
    p << "(";
    auto emitTypedOperand = [&](Value operand, bool isSigned) {
      if (isSigned) {
        emitFixedWidthType(p, getWidth(operand.getType()), true);
        p << "(";
        p.getInlinable(operand).emit();
        p << ")";
      } else {
        emitOperand(operand, getPrecedence(op), p);
      }
    };
    emitTypedOperand(op.getLhs(), isSignedOperation(op));
    p << getOperator(op);
    emitTypedOperand(op.getRhs(), isa<DivSOp, ModSOp>(op));
    p << ")";
  }
};

struct ICmpEmitter : OpEmissionPattern<ICmpOp> {
  using OpEmissionPattern::OpEmissionPattern;

  MatchResult matchInlinable(Value value) override {
    auto op = value.getDefiningOp<ICmpOp>();
    if (!op || getWidth(op.getLhs().getType()) > 512)
      return {};
    return Precedence::RELATIONAL;
  }

  void emitInlined(Value value, EmissionPrinter &p) override {
    auto op = value.getDefiningOp<ICmpOp>();
    bool isSigned = ICmpOp::isPredicateSigned(op.getPredicate());
    auto emitTypedOperand = [&](Value operand) {
      emitFixedWidthType(p, getWidth(operand.getType()), isSigned);
      p << "(";
      p.getInlinable(operand).emit();
      p << ")";
    };

    emitTypedOperand(op.getLhs());
    // HW integers are two-state values at this point. Case and wildcard
    // equality therefore have the same behavior as ordinary equality.
    switch (op.getPredicate()) {
    case ICmpPredicate::eq:
    case ICmpPredicate::ceq:
    case ICmpPredicate::weq:
      p << " == ";
      break;
    case ICmpPredicate::ne:
    case ICmpPredicate::cne:
    case ICmpPredicate::wne:
      p << " != ";
      break;
    case ICmpPredicate::slt:
    case ICmpPredicate::ult:
      p << " < ";
      break;
    case ICmpPredicate::sle:
    case ICmpPredicate::ule:
      p << " <= ";
      break;
    case ICmpPredicate::sgt:
    case ICmpPredicate::ugt:
      p << " > ";
      break;
    case ICmpPredicate::sge:
    case ICmpPredicate::uge:
      p << " >= ";
      break;
    default:
      llvm_unreachable("unsupported comparison predicate");
    }
    emitTypedOperand(op.getRhs());
  }
};

struct MuxEmitter : OpEmissionPattern<MuxOp> {
  using OpEmissionPattern::OpEmissionPattern;

  MatchResult matchInlinable(Value value) override {
    auto op = value.getDefiningOp<MuxOp>();
    if (!op || !isa<IntegerType>(value.getType()) ||
        getWidth(value.getType()) > 512)
      return {};
    return Precedence::FUNCTIONAL_CAST;
  }

  void emitInlined(Value value, EmissionPrinter &p) override {
    auto op = value.getDefiningOp<MuxOp>();
    unsigned width = getWidth(value.getType());
    emitFixedWidthType(p, width);
    p << "(";
    emitOperand(op.getCond(), Precedence::TERNARY, p);
    p << " ? ";
    emitFixedWidthType(p, width);
    p << "(";
    p.getInlinable(op.getTrueValue()).emit();
    p << ")";
    p << " : ";
    emitFixedWidthType(p, width);
    p << "(";
    p.getInlinable(op.getFalseValue()).emit();
    p << ")";
    p << ")";
  }
};

struct ConcatEmitter : OpEmissionPattern<ConcatOp> {
  using OpEmissionPattern::OpEmissionPattern;

  MatchResult matchInlinable(Value value) override {
    auto op = value.getDefiningOp<ConcatOp>();
    if (!op || getWidth(value.getType()) > 512)
      return {};
    return Precedence::FUNCTIONAL_CAST;
  }

  void emitInlined(Value value, EmissionPrinter &p) override {
    auto op = value.getDefiningOp<ConcatOp>();
    emitFixedWidthType(p, getWidth(value.getType()));
    p << "(";
    for (size_t i = 0, e = op.getInputs().size(); i < e; ++i) {
      if (i + 1 != e)
        p << "sc_dt::concat(";
      emitUnsignedCast(op.getInputs()[i], p);
      if (i + 1 != e)
        p << ", ";
    }
    for (size_t i = 1, e = op.getInputs().size(); i < e; ++i)
      p << ")";
    p << ")";
  }
};

struct ExtractEmitter : OpEmissionPattern<ExtractOp> {
  using OpEmissionPattern::OpEmissionPattern;

  MatchResult matchInlinable(Value value) override {
    auto op = value.getDefiningOp<ExtractOp>();
    if (!op || getWidth(value.getType()) > 512)
      return {};
    return Precedence::FUNCTIONAL_CAST;
  }

  void emitInlined(Value value, EmissionPrinter &p) override {
    auto op = value.getDefiningOp<ExtractOp>();
    unsigned low = op.getLowBit();
    unsigned high = low + getWidth(value.getType()) - 1;
    emitFixedWidthType(p, getWidth(value.getType()));
    p << "(";
    emitUnsignedCast(op.getInput(), p);
    p << ".range(" << high << ", " << low << "))";
  }
};

struct ParityEmitter : OpEmissionPattern<ParityOp> {
  using OpEmissionPattern::OpEmissionPattern;

  MatchResult matchInlinable(Value value) override {
    auto op = value.getDefiningOp<ParityOp>();
    if (!op || getWidth(op.getInput().getType()) > 512)
      return {};
    return Precedence::FUNCTION_CALL;
  }

  void emitInlined(Value value, EmissionPrinter &p) override {
    auto op = value.getDefiningOp<ParityOp>();
    emitUnsignedCast(op.getInput(), p);
    p << ".xor_reduce()";
  }
};

struct ReplicateEmitter : OpEmissionPattern<ReplicateOp> {
  using OpEmissionPattern::OpEmissionPattern;

  MatchResult matchInlinable(Value value) override {
    auto op = value.getDefiningOp<ReplicateOp>();
    if (!op || getWidth(value.getType()) > 512)
      return {};
    return Precedence::FUNCTIONAL_CAST;
  }

  void emitInlined(Value value, EmissionPrinter &p) override {
    auto op = value.getDefiningOp<ReplicateOp>();
    emitFixedWidthType(p, getWidth(value.getType()));
    p << "(";
    for (size_t i = 1, e = op.getMultiple(); i < e; ++i)
      p << "sc_dt::concat(";
    emitUnsignedCast(op.getInput(), p);
    for (size_t i = 1, e = op.getMultiple(); i < e; ++i) {
      p << ", ";
      emitUnsignedCast(op.getInput(), p);
      p << ")";
    }
    p << ")";
  }
};

} // namespace

void circt::ExportSystemC::populateCombOpEmitters(
    OpEmissionPatternSet &patterns, MLIRContext *context) {
  patterns
      .add<VariadicExpressionEmitter<AddOp>, VariadicExpressionEmitter<MulOp>,
           VariadicExpressionEmitter<AndOp>, VariadicExpressionEmitter<OrOp>,
           VariadicExpressionEmitter<XorOp>, BinaryExpressionEmitter<SubOp>,
           BinaryExpressionEmitter<DivUOp>, BinaryExpressionEmitter<DivSOp>,
           BinaryExpressionEmitter<ModUOp>, BinaryExpressionEmitter<ModSOp>,
           BinaryExpressionEmitter<ShlOp>, BinaryExpressionEmitter<ShrUOp>,
           BinaryExpressionEmitter<ShrSOp>, ICmpEmitter, MuxEmitter,
           ConcatEmitter, ExtractEmitter>(context);
  patterns.add<ParityEmitter, ReplicateEmitter>(context);
}
