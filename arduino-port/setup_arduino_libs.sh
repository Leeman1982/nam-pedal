#!/usr/bin/env bash
# setup_arduino_libs.sh — create the NAMCore Arduino library from the git submodules.
# Run from the repo root AFTER: git submodule update --init --recursive
#
# Files are COPIED (not symlinked): Arduino IDE / arduino-cli do not follow
# symlinks when indexing libraries.  Re-run this script after updating the
# submodules.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARDUINO_LIBS="${ARDUINO_LIBRARIES_PATH:-$HOME/Arduino/libraries}"

echo "Repo root  : $REPO_ROOT"
echo "Arduino lib: $ARDUINO_LIBS"

NAM_CORE="$REPO_ROOT/NeuralAmpModelerCore"
NAMB="$REPO_ROOT/nam-binary-loader"

if [ ! -f "$NAM_CORE/NAM/dsp.h" ]; then
  echo ""
  echo "ERROR: NeuralAmpModelerCore submodule not initialised."
  echo "Run:  git submodule update --init --recursive"
  exit 1
fi

DEST="$ARDUINO_LIBS/NAMCore"
rm -rf "$DEST"
mkdir -p "$DEST/src"

# NAM inference engine.
# container.{h,cpp} (SlimmableContainer host API) uses std::mutex which does
# not exist on bare-metal ARM — and the pedal loads plain WaveNet models, so
# it isn't needed.  Nothing else in NAM includes it.
cp -r "$NAM_CORE/NAM" "$DEST/src/NAM"
rm -f "$DEST/src/NAM/container.h" "$DEST/src/NAM/container.cpp"

# get_dsp.cpp also uses std::mutex (version-support registry locking).
# The firmware is single-threaded, so strip the locking.
sed -e 's|#include <mutex>||' \
    -e '/^std::mutex& version_support_registry_mutex()/,/^}/d' \
    -e '/std::lock_guard<std::mutex>/d' \
    "$NAM_CORE/NAM/get_dsp.cpp" > "$DEST/src/NAM/get_dsp.cpp"

# SlimmableWavenet (live channel switching for desktop hosts) uses
# std::atomic<std::shared_ptr> which has no bare-metal implementation.
# The pedal loads fixed-size WaveNet .namb models, so drop it and patch the
# one reference in wavenet/model.cpp to error out instead.
rm -f "$DEST/src/NAM/slimmable.h" \
      "$DEST/src/NAM/wavenet/slimmable.h" "$DEST/src/NAM/wavenet/slimmable.cpp"
sed -e 's|#include "slimmable.h"||' \
    -e 's|return nam::slimmable_wavenet::create_config(config, sampleRate);|throw std::runtime_error("Slimmable models not supported in this firmware");|' \
    "$NAM_CORE/NAM/wavenet/model.cpp" > "$DEST/src/NAM/wavenet/model.cpp"

# Eigen (header-only). NAM includes <Eigen/Dense>, so the Eigen/ directory
# must sit directly inside src/.
cp -r "$NAM_CORE/Dependencies/eigen/Eigen" "$DEST/src/Eigen"

# nlohmann json single header. NAM includes it as "json.hpp".
# Strip trailing NOLINT comments from the version #defines: the ARM GCC
# shipped with STM32duino pastes them into macro tokens and errors out.
sed 's|// NOLINT(modernize-macro-to-enum)||' \
    "$NAM_CORE/Dependencies/nlohmann/json.hpp" > "$DEST/src/json.hpp"

# namb binary loader. get_dsp_namb.cpp in the submodule includes
# <NAM/wavenet.h>, which no longer exists (wavenet moved to
# NAM/wavenet/model.h in a NeuralAmpModelerCore update) — patch it here.
mkdir -p "$DEST/src/namb"
cp "$NAMB/namb/binary_parser_registry.h" "$DEST/src/namb/"
cp "$NAMB/namb/get_dsp_namb.h"           "$DEST/src/namb/"
cp "$NAMB/namb/namb_format.h"            "$DEST/src/namb/"
sed 's|<NAM/wavenet\.h>|<NAM/wavenet/model.h>|' \
    "$NAMB/namb/get_dsp_namb.cpp" > "$DEST/src/namb/get_dsp_namb.cpp"

# Umbrella header — Arduino's include scanner only matches headers at the top
# level of src/, so the sketch includes <NAMCore.h> to activate this library.
cat > "$DEST/src/NAMCore.h" <<EOF
#pragma once
// Umbrella header for the NAMCore Arduino library.
// Including this activates the library and puts NAM/, namb/, Eigen/ and
// json.hpp on the include path.

// Arduino defines several short macros that collide with Eigen identifiers:
//   B0/B1 (binary.h)  — Eigen matrix variable names (BDCSVD)
//   F()   (WString.h) — Eigen template parameter F in result_of<F(Args...)>
// Safe to undef here: this sketch does not use F("...") flash strings, and
// 0b0/0b1 literals replace B0/B1.
#ifdef B0
  #undef B0
#endif
#ifdef B1
  #undef B1
#endif
#ifdef F
  #undef F
#endif

#include <NAM/dsp.h>
#include <NAM/activations.h>
#include <namb/get_dsp_namb.h>
EOF

# library.properties
cat > "$DEST/library.properties" <<EOF
name=NAMCore
version=1.0.0
author=Steven Atkinson / tone-3000
sentence=Neural Amp Modeler inference engine + .namb binary loader.
paragraph=NeuralAmpModelerCore + nam-binary-loader, packaged for Arduino IDE.
category=Signal Processing
url=https://github.com/sdatkinson/NeuralAmpModelerCore
architectures=*
includes=NAMCore.h
EOF

# ── Enable C++ exceptions for the STM32duino platform ────────────────────────
# NAM uses C++ exceptions, but STM32duino's platform.txt appends
# -fno-exceptions AFTER the build_opt.h flags, so build_opt.h alone cannot
# enable them.  platform.local.txt is the documented override: its
# compiler.cpp.extra_flags is appended after -fno-exceptions and wins.
enable_exceptions() {
  local pkgs="$1"
  [ -d "$pkgs" ] || return 0
  local found=0
  for plat in "$pkgs"/packages/STMicroelectronics/hardware/stm32/*/; do
    [ -f "$plat/platform.txt" ] || continue
    echo "compiler.cpp.extra_flags=-fexceptions" > "$plat/platform.local.txt"
    echo "Exceptions enabled: $plat/platform.local.txt"
    found=1
  done
  return 0
}
enable_exceptions "$HOME/.arduino15"
enable_exceptions "${LOCALAPPDATA:-}/Arduino15"      # Windows (Git Bash)
enable_exceptions "$HOME/Library/Arduino15"          # macOS

echo ""
echo "NAMCore library created at: $DEST"
echo ""
echo "Also install via Library Manager:"
echo "  - U8g2  (by Oliver Kraus)"
echo ""
echo "NOTE: if you install or update the STM32 board package AFTER running"
echo "this script, run the script again (the exceptions flag is written into"
echo "the board package folder and is lost on update)."
