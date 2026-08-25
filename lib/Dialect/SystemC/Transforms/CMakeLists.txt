add_circt_dialect_library(CIRCTSystemCTransforms
  SystemCContainerInteropLowering.cpp
  SystemCLowerInstanceInterop.cpp
  SystemCWrapVerilatedInstances.cpp

  DEPENDS
  CIRCTSystemCTransformsIncGen

  LINK_LIBS PUBLIC
  CIRCTSystemC
  CIRCTInteropDialect
  CIRCTSupport
  MLIRIR
  MLIRFuncDialect
  MLIRPass
  MLIRTransformUtils
)
