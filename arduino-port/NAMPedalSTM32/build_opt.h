# build_opt.h — extra compiler flags picked up automatically by STM32duino.
# This file is NOT included as a C++ header; it is read by the build system.
# Each non-comment line is appended to the compiler command line.
-DNAM_SAMPLE_FLOAT
-DNAM_USE_INLINE_GEMM
-O3
-ffast-math
-funroll-loops
-ftree-vectorize
-fexceptions
