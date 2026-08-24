//===- SimEmissionPatterns.h - Sim emission patterns --------*- C++ -*-===-//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_TARGET_EXPORTSYSTEMC_SIMEMISSIONPATTERNS_H
#define CIRCT_TARGET_EXPORTSYSTEMC_SIMEMISSIONPATTERNS_H

#include "../EmissionPatternSupport.h"

namespace circt {
namespace ExportSystemC {

void populateSimOpEmitters(OpEmissionPatternSet &patterns,
                           MLIRContext *context);

} // namespace ExportSystemC
} // namespace circt

#endif // CIRCT_TARGET_EXPORTSYSTEMC_SIMEMISSIONPATTERNS_H
