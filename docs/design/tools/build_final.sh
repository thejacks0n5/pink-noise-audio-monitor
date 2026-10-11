#!/bin/sh
# Builds design/final/: the asset catalog for icon A and its compiled output.
# Needs Xcode's actool (xcrun). Run after tools/gen_icons.py A.
set -e
cd "$(dirname "$0")/.."
A=icons/A
SET=final/Assets.xcassets/AppIcon.appiconset
rm -rf final/Assets.xcassets final/Assets.car final/AppIcon.icns final/AppIcon-partial-Info.plist
mkdir -p "$SET"
cat > final/Assets.xcassets/Contents.json <<'J'
{
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
J
# 16 and 32 px slots: the hand-fitted pixel masters. 64 px and up: the template-shape master.
cp $A/A_16.png   "$SET/icon_16x16.png"
cp $A/A_32.png   "$SET/icon_16x16@2x.png"
cp $A/A_32.png   "$SET/icon_32x32.png"
cp $A/A_64.png   "$SET/icon_32x32@2x.png"
cp $A/A_128.png  "$SET/icon_128x128.png"
cp $A/A_256.png  "$SET/icon_128x128@2x.png"
cp $A/A_256.png  "$SET/icon_256x256.png"
cp $A/A_512.png  "$SET/icon_256x256@2x.png"
cp $A/A_512.png  "$SET/icon_512x512.png"
cp $A/A_1024.png "$SET/icon_512x512@2x.png"
cat > "$SET/Contents.json" <<'J'
{
  "images" : [
    { "idiom" : "mac", "size" : "16x16",   "scale" : "1x", "filename" : "icon_16x16.png" },
    { "idiom" : "mac", "size" : "16x16",   "scale" : "2x", "filename" : "icon_16x16@2x.png" },
    { "idiom" : "mac", "size" : "32x32",   "scale" : "1x", "filename" : "icon_32x32.png" },
    { "idiom" : "mac", "size" : "32x32",   "scale" : "2x", "filename" : "icon_32x32@2x.png" },
    { "idiom" : "mac", "size" : "128x128", "scale" : "1x", "filename" : "icon_128x128.png" },
    { "idiom" : "mac", "size" : "128x128", "scale" : "2x", "filename" : "icon_128x128@2x.png" },
    { "idiom" : "mac", "size" : "256x256", "scale" : "1x", "filename" : "icon_256x256.png" },
    { "idiom" : "mac", "size" : "256x256", "scale" : "2x", "filename" : "icon_256x256@2x.png" },
    { "idiom" : "mac", "size" : "512x512", "scale" : "1x", "filename" : "icon_512x512.png" },
    { "idiom" : "mac", "size" : "512x512", "scale" : "2x", "filename" : "icon_512x512@2x.png" }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
J
xcrun actool final/Assets.xcassets \
    --compile final \
    --platform macosx \
    --minimum-deployment-target 12.0 \
    --app-icon AppIcon \
    --output-partial-info-plist final/AppIcon-partial-Info.plist \
    --output-format human-readable-text --errors --warnings --notices
cp $A/A_1024.png final/icon_1024.png
