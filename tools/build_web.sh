#!/usr/bin/env bash
# Build the firmware and package it for the web flasher in web/.
#   ./tools/build_web.sh            then:  python3 -m http.server -d web 8000
# and open http://localhost:8000 in Chrome/Edge (localhost counts as secure for Web Serial).
# Needs: pip install platformio "esptool<5"
# Optional: CS_UPDATE_URL=https://you.github.io/claudestick/ bakes the OTA source into the firmware.
set -euo pipefail
cd "$(dirname "$0")/.."

(cd firmware && pio run)   # embed_files.py reads CS_UPDATE_URL from the environment

B=firmware/.pio/build/m5stack-sticks3
BOOT_APP0=$(find "${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoespressif32" -name boot_app0.bin | head -1)
[ -n "$BOOT_APP0" ] || { echo "boot_app0.bin not found"; exit 1; }

mkdir -p web/firmware
python3 -m esptool --chip esp32s3 merge_bin -o web/firmware/claudestick.bin \
  --flash_mode keep --flash_freq keep --flash_size 8MB \
  0x0 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$B/firmware.bin"

# App-only image for over-the-air updates from the Stick's settings page.
cp "$B/firmware.bin" web/firmware/claudestick-app.bin

VERSION=$(sed -n 's/.*FW_VERSION *= *"\([^"]*\)".*/\1/p' firmware/src/main.cpp)
python3 - "$VERSION" <<'PY'
import hashlib, json, sys
v = sys.argv[1]
p = "web/manifest.json"
m = json.load(open(p)); m["version"] = v
json.dump(m, open(p, "w"), indent=2); open(p, "a").write("\n")
app = "firmware/claudestick-app.bin"
sha = hashlib.sha256(open("web/" + app, "rb").read()).hexdigest()
# Changelog bullets for this version (shown on the Stick's update prompt).
notes, grab = [], False
for line in open("CHANGELOG.md", encoding="utf-8"):
    if line.startswith("## "):
        grab = line[3:].strip() == v
        continue
    if grab and line.strip():
        notes.append(line.rstrip())
json.dump({"version": v, "app": app, "sha256": sha, "notes": "\n".join(notes)}, open("web/ota.json", "w"), indent=2)
PY
ls -la web/firmware/claudestick.bin
echo "web flasher ready for firmware $VERSION"
