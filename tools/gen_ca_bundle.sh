#!/usr/bin/env bash
# Regenerate firmware/data/cert/x509_crt_bundle.bin (Mozilla roots via certifi, plus firmware/data/cert/extra_roots.pem)
# for HTTPS update checks and the bridge connection.
# Needs: pip install certifi cryptography; and gen_crt_bundle.py from arduino-esp32/tools.
# (tools/ca_bundle.py can add the extra roots to an existing bundle without PlatformIO.)
set -euo pipefail
cd "$(dirname "$0")/.."
GEN=${GEN_CRT_BUNDLE:-$(find "${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoespressif32" -name gen_crt_bundle.py | head -1)}
[ -n "$GEN" ] || { echo "gen_crt_bundle.py not found; set GEN_CRT_BUNDLE"; exit 1; }
tmp=$(mktemp -d)
cat "$(python3 -c 'import certifi; print(certifi.where())')" firmware/data/cert/extra_roots.pem > "$tmp/roots.pem"
(cd "$tmp" && python3 "$GEN" -i "$tmp/roots.pem")
mkdir -p firmware/data/cert && cp "$tmp/x509_crt_bundle" firmware/data/cert/x509_crt_bundle.bin
echo "wrote firmware/data/cert/x509_crt_bundle.bin"
