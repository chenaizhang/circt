//===- SimEmissionPatterns.cpp - Sim emission patterns --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SimEmissionPatterns.h"
#include "../EmissionPrinter.h"
#include "circt/Dialect/Sim/SimOps.h"

using namespace circt;
using namespace circt::sim;
using namespace circt::ExportSystemC;

namespace {

/// Emit a literal format fragment as a C++ string literal.  The diagnostic
/// operations produced by the Verilog importer intentionally retain their
/// format-string SSA values until the SystemC target, so they can preserve
/// useful RTL error messages without affecting datapath behavior.
struct FormatLiteralEmitter : OpEmissionPattern<FormatLiteralOp> {
  using OpEmissionPattern::OpEmissionPattern;

  MatchResult matchInlinable(Value value) override {
    if (value.getDefiningOp<FormatLiteralOp>())
      return Precedence::LIT;
    return {};
  }

  void emitInlined(Value value, EmissionPrinter &p) override {
    auto op = value.getDefiningOp<FormatLiteralOp>();
    p.emitAttr(op.getLiteralAttr());
  }
};

/// Emit the common non-procedural simulation print form.  The SystemVerilog
/// clock is already represented as an input/channel by the HW-to-SystemC
/// conversion when available.  For a scalar clock value, the surrounding
/// lowering has already materialized the event guard and the condition alone
/// is emitted here.
struct PrintEmitter : OpEmissionPattern<PrintFormattedOp> {
  using OpEmissionPattern::OpEmissionPattern;

  void emitStatement(PrintFormattedOp op, EmissionPrinter &p) override {
    p << "if (";
    p.getInlinable(op.getCondition()).emit();
    p << ") SC_REPORT_ERROR(\"circt.sim\", ";
    p.getInlinable(op.getInput()).emit();
    p << ");\n";
  }
};

/// Procedural print is unconditional within the containing process.
struct ProceduralPrintEmitter : OpEmissionPattern<PrintFormattedProcOp> {
  using OpEmissionPattern::OpEmissionPattern;

  void emitStatement(PrintFormattedProcOp op, EmissionPrinter &p) override {
    p << "SC_REPORT_ERROR(\"circt.sim\", ";
    p.getInlinable(op.getInput()).emit();
    p << ");\n";
  }
};

} // namespace

void circt::ExportSystemC::populateSimOpEmitters(
    OpEmissionPatternSet &patterns, MLIRContext *context) {
  patterns.add<FormatLiteralEmitter, PrintEmitter, ProceduralPrintEmitter>(
      context);
}
