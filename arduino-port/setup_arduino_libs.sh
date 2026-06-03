#!/usr/bin/env bash
# setup_arduino_libs.sh — create the NAMCore Arduino library from the git submodules.
# Run from the repo root AFTER: git submodule update --init --recursive

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
mkdir -p "$DEST/src"

# Create symlinks (Linux/macOS).  On Windows, run as administrator or
# enable Developer Mode, then use mklink /D instead.
ln -sfn "$NAM_CORE/NAM"          "$DEST/src/NAM"
ln -sfn "$NAM_CORE/Dependencies" "$DEST/src/Dependencies"
ln -sfn "$NAMB/namb"             "$DEST/src/namb"

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
EOF

echo ""
echo "NAMCore library created at: $DEST"
echo ""
echo "Ensure the following include paths are added in Arduino IDE:"
echo "  Sketch > Include Library > Manage Libraries (already done via library.properties)"
echo ""
echo "Also install via Library Manager:"
echo "  - audio-tools  (by Phil Schatzmann)"
echo "  - U8g2         (by Oliver Kraus)"
