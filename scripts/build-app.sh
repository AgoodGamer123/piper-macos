#!/bin/bash
#
# Builds Piper.app into build/. Needs Xcode or the Command Line Tools,
# nothing else: libratbag is compiled as part of the Swift package.
#
#   scripts/build-app.sh                 universal (Apple Silicon + Intel)
#   ARCHS=arm64 scripts/build-app.sh     faster, this Mac's architecture only
#   VERSION=1.2.3 scripts/build-app.sh   version shown in About Piper

set -euo pipefail
cd "$(dirname "$0")/.."

ARCHS="${ARCHS:-arm64 x86_64}"
if [ -z "${VERSION:-}" ]; then
	VERSION="$(git describe --tags --always 2>/dev/null || echo 0.0.0)"
	VERSION="${VERSION#v}"
fi
BUILD_NUMBER="${BUILD_NUMBER:-$(git rev-list --count HEAD 2>/dev/null || echo 1)}"
APP=build/Piper.app

arch_flags=()
for a in $ARCHS; do arch_flags+=(--arch "$a"); done

echo "==> Compiling ($ARCHS)"
swift build -c release "${arch_flags[@]}"
BIN="$(swift build -c release "${arch_flags[@]}" --show-bin-path)"

echo "==> Assembling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN/Piper" "$APP/Contents/MacOS/Piper"

# device descriptions for libratbag, and Piper's device pictures
mkdir -p "$APP/Contents/Resources/devices"
cp libratbag/data/devices/*.device "$APP/Contents/Resources/devices/"
cp -R Resources/svgs "$APP/Contents/Resources/svgs"
iconutil -c icns Resources/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"

sed -e "s/@VERSION@/$VERSION/g" -e "s/@BUILD@/$BUILD_NUMBER/g" Resources/Info.plist.in \
	> "$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" >/dev/null

# Ad-hoc signature: required to run on Apple Silicon, not a Developer ID
codesign --force --sign - --timestamp=none "$APP"
codesign --verify --strict "$APP"

echo "==> Built $APP ($VERSION, $(lipo -archs "$APP/Contents/MacOS/Piper"))"
