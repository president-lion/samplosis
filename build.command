#!/bin/zsh
# Builds Samplosis for macOS (CLAP, VST3 and Audio Unit) and installs it in ~/Library/Audio/Plug-Ins.
# Double-click it in Finder, or run it in Terminal. Needs the Command Line Tools and: brew install cmake ninja
cd "$(dirname "$0")" || exit 1
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
fail() { echo "$1"; read -r "?Press Return to close. "; exit 1; }
command -v cmake >/dev/null || fail "CMake isn't installed. In Terminal: brew install cmake ninja"
command -v ninja >/dev/null || fail "Ninja isn't installed. In Terminal: brew install cmake ninja"
xcode-select -p >/dev/null 2>&1 || fail "The Command Line Tools aren't installed. In Terminal: xcode-select --install"
echo "Building Samplosis (the first time downloads its SDKs and takes a few minutes)..."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null || fail "CMake couldn't set up the build."
cmake --build build || fail "The build failed; the messages above say why."
A=build/samplosis_assets
P="$HOME/Library/Audio/Plug-Ins"
mkdir -p "$P/CLAP" "$P/VST3" "$P/Components"
for b in Samplosis.clap:CLAP Samplosis.vst3:VST3 Samplosis.component:Components; do
  name=${b%%:*}; dir=${b##*:}
  codesign --force --deep -s - "$A/$name" 2>/dev/null
  rm -rf "$P/$dir/$name" && cp -R "$A/$name" "$P/$dir/" || fail "Couldn't copy $name to $P/$dir (is the DAW open?)"
done
killall -9 AudioComponentRegistrar 2>/dev/null   # so the Audio Unit is seen without logging out
echo "Installed Samplosis.clap, Samplosis.vst3 and Samplosis.component in $P."
echo "Rescan plugins in your DAW (or restart it) to find Samplosis."
read -r "?Press Return to close. "
