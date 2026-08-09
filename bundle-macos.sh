#!/bin/sh
# Bundle tivi as a self-contained macOS app — produces dist/macos-arm64/tivi.app
# (build/ keeps the intermediates; dist/ mirrors the Windows installer layout).
#   - builds the binary (build-macos.sh)
#   - packs assets/tivi.ico's PNG frames (16 → 256 px) into a .icns
#   - writes Info.plist with media-type associations (Finder "Open With"
#     delivery is handled in-app via the Apple Event → single-instance route)
#   - copies the Homebrew dylib closure (FFmpeg, libass, raylib, …) into
#     Contents/Frameworks and rewrites install names, so the app runs on Macs
#     without Homebrew
#   - ad-hoc codesigns everything (distribution to strangers additionally
#     needs a Developer ID + notarization)
set -e
cd "$(dirname "$0")"

VERSION=$(sed -n 's/#define TIVI_VERSION "\(.*\)"/\1/p' src/main.c)
sh build-macos.sh

APP=dist/macos-arm64/tivi.app
FW="$APP/Contents/Frameworks"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$FW"
cp build/tivi "$APP/Contents/MacOS/tivi"

# ---- icon: the .ico's PNG frames → iconset → icns ----
# ffmpeg stream order in assets/tivi.ico: 16,24,32,48,64,128,256
echo 'building icns'
ICONSET=build/tivi.iconset
rm -rf "$ICONSET"; mkdir -p "$ICONSET"
extract() { ffmpeg -y -loglevel error -i assets/tivi.ico -map "0:v:$1" "$ICONSET/$2"; }
extract 0 icon_16x16.png
extract 2 icon_16x16@2x.png
extract 2 icon_32x32.png
extract 4 icon_32x32@2x.png
extract 5 icon_128x128.png
extract 6 icon_128x128@2x.png
extract 6 icon_256x256.png
iconutil -c icns -o "$APP/Contents/Resources/tivi.icns" "$ICONSET"

# ---- Info.plist ----
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>       <string>tivi</string>
    <key>CFBundleIdentifier</key>       <string>io.tivi.tivi</string>
    <key>CFBundleName</key>             <string>tivi</string>
    <key>CFBundleDisplayName</key>      <string>tivi</string>
    <key>CFBundleShortVersionString</key> <string>$VERSION</string>
    <key>CFBundleVersion</key>          <string>$VERSION</string>
    <key>CFBundlePackageType</key>      <string>APPL</string>
    <key>CFBundleIconFile</key>         <string>tivi</string>
    <key>LSMinimumSystemVersion</key>   <string>11.0</string>
    <key>NSHighResolutionCapable</key>  <true/>
    <key>LSApplicationCategoryType</key> <string>public.app-category.video</string>
    <key>CFBundleDocumentTypes</key>
    <array>
        <dict>
            <key>CFBundleTypeName</key>      <string>Video File</string>
            <key>CFBundleTypeRole</key>      <string>Viewer</string>
            <key>LSHandlerRank</key>         <string>Alternate</string>
            <key>CFBundleTypeExtensions</key>
            <array>
                <string>mp4</string> <string>mkv</string> <string>webm</string>
                <string>avi</string> <string>mov</string> <string>m4v</string>
                <string>wmv</string> <string>flv</string> <string>mpg</string>
                <string>mpeg</string> <string>ts</string> <string>m2ts</string>
                <string>mts</string> <string>vob</string> <string>ogv</string>
                <string>3gp</string> <string>divx</string>
            </array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key>      <string>Audio File</string>
            <key>CFBundleTypeRole</key>      <string>Viewer</string>
            <key>LSHandlerRank</key>         <string>Alternate</string>
            <key>CFBundleTypeExtensions</key>
            <array>
                <string>mp3</string> <string>flac</string> <string>wav</string>
                <string>ogg</string> <string>opus</string> <string>m4a</string>
                <string>aac</string> <string>wma</string> <string>ac3</string>
            </array>
        </dict>
    </array>
</dict>
</plist>
PLIST

# ---- self-contained: copy the Homebrew dylib closure, rewrite install names ----
# Deps come in two shapes: absolute /opt/homebrew paths, and @rpath/@loader_path
# names that Homebrew dylibs use for their siblings (resolved via LC_RPATH,
# which breaks once the file moves into the bundle) — those are resolved
# against the referring dylib's ORIGINAL directory, so the walk carries it.
echo 'bundling dylibs (FFmpeg tree — this takes a moment)'
bundle_deps() {  # $1 = Mach-O file to fix   $2 = its original source directory
    otool -L "$1" | awk 'NR>1 {print $1}' |
    grep -E '^/opt/homebrew|^/usr/local/(opt|Cellar|lib)|^@rpath/|^@loader_path/' |
    while read -r dep; do
        base=$(basename "$dep")
        case "$dep" in
            @rpath/*|@loader_path/*)
                src="$2/$base"
                [ -f "$src" ] || src="$2/../lib/$base"
                [ -f "$src" ] || { echo "warning: cannot resolve $dep (from $1)"; continue; } ;;
            *) src="$dep" ;;
        esac
        if [ ! -f "$FW/$base" ]; then
            cp -L "$src" "$FW/$base"
            chmod u+w "$FW/$base"
            install_name_tool -id "@executable_path/../Frameworks/$base" "$FW/$base" 2>/dev/null
            bundle_deps "$FW/$base" "$(cd "$(dirname "$src")" && pwd -P)"
        fi
        install_name_tool -change "$dep" "@executable_path/../Frameworks/$base" "$1" 2>/dev/null
    done
}
bundle_deps "$APP/Contents/MacOS/tivi" "$(pwd)"

# ---- ad-hoc sign (install_name_tool invalidated the linker signatures) ----
echo 'codesigning (ad-hoc)'
find "$FW" -name '*.dylib' -exec codesign --force -s - {} \; 2>/dev/null
codesign --force -s - "$APP/Contents/MacOS/tivi" 2>/dev/null
codesign --force -s - "$APP" 2>/dev/null

echo "bundled $(pwd)/$APP  ($(du -sh "$APP" | cut -f1))"
