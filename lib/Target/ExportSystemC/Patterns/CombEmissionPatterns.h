//===- CombEmissionPatterns.h - Comb emission patterns ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_TARGET_EXPORTSYSTEMC_COMBEMISSIONPATTERNS_H
#define CIRCT_TARGET_EXPORTSYSTEMC_COMBEMISSIONPATTERNS_H

#include "../EmissionPatternSupport.h"

namespace circt {
namespace ExportSystemC {

void populateCombOpEmitters(OpEmissionPatternSet &patterns,
                            MLIRContext *context);

} // namespace ExportSystemC
} // namespace circt

#endif // CIRCT_TARGET_EXPORTSYSTEMC_COMBEMISSIONPATTERNS_H
