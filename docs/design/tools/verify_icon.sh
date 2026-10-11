#!/bin/sh
# Puts final/Assets.car + final/AppIcon.icns into a throwaway .app (never launched)
# and saves what macOS draws for it at each size into verify/system_renders/.
# A fresh bundle name and identifier are used on every run so the icon cache cannot answer.
set -e
cd "$(dirname "$0")/.."
TAG=$(date +%H%M%S)
APP="verify/PNAMIconCheck_$TAG.app"
rm -rf verify && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" verify/system_renders
cp final/Assets.car final/AppIcon.icns "$APP/Contents/Resources/"
printf '#!/bin/sh\nexit 0\n' > "$APP/Contents/MacOS/PNAMIconCheck" && chmod +x "$APP/Contents/MacOS/PNAMIconCheck"
ICONFILE=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIconFile' final/AppIcon-partial-Info.plist)
ICONNAME=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIconName' final/AppIcon-partial-Info.plist)
cat > "$APP/Contents/Info.plist" <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>design.verify.pnamiconcheck.t$TAG</string>
<key>CFBundleName</key><string>PNAMIconCheck</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleExecutable</key><string>PNAMIconCheck</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>12.0</string>
<key>CFBundleIconFile</key><string>$ICONFILE</string>
<key>CFBundleIconName</key><string>$ICONNAME</string>
</dict></plist>
P
swift tools/icon_probe.swift verify/system_renders "$PWD/$APP" 16 32 64 128 256 512 1024
echo "$APP" > verify/bundle_used.txt

# Control: the same bundle with ONLY the fallback AppIcon.icns (no Assets.car, no
# CFBundleIconName). This is what the system shows if the catalog is not picked up.
APP2="verify/PNAMIconCheckIcnsOnly_$TAG.app"
mkdir -p "$APP2/Contents/MacOS" "$APP2/Contents/Resources" verify/icns_only_renders
cp final/AppIcon.icns "$APP2/Contents/Resources/"
cp "$APP/Contents/MacOS/PNAMIconCheck" "$APP2/Contents/MacOS/PNAMIconCheck"
sed -e "s/pnamiconcheck.t$TAG/pnamiconcheck.icnsonly.t$TAG/" \
    -e "s#<key>CFBundleIconName</key><string>$ICONNAME</string>##" "$APP/Contents/Info.plist" > "$APP2/Contents/Info.plist"
swift tools/icon_probe.swift verify/icns_only_renders "$PWD/$APP2" 16 32 64 128 256 512 1024
