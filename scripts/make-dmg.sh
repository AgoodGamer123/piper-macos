#!/bin/bash
#
# Packs build/Piper.app into build/Piper-<version>.dmg with the usual
# "drag to Applications" layout.

set -euo pipefail
cd "$(dirname "$0")/.."

APP=build/Piper.app
[ -d "$APP" ] || { echo "Run scripts/build-app.sh first" >&2; exit 1; }
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")"
DMG="build/Piper-$VERSION.dmg"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"

rm -f "$DMG"
hdiutil create -volname "Piper $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG" >/dev/null
echo "==> $DMG ($(du -h "$DMG" | cut -f1))"
