#!/usr/bin/env bash
# Builds one host-side test: tests/host/build.sh <source.cpp> <output-binary>
# The firmware runs here against small Arduino/M5/ESP stand-ins (tests/host/stub), so logic and screens
# can be tested without a board. Needs g++ and the ArduinoJson headers (found via $ARDUINOJSON_DIR,
# else cloned into tests/host/.cache once).
set -euo pipefail
cd "$(dirname "$0")"
REPO=../..
AJ="${ARDUINOJSON_DIR:-.cache/ArduinoJson}"
if [ ! -f "$AJ/src/ArduinoJson.h" ]; then
  mkdir -p .cache
  git clone -q --depth 1 --branch v7.4.2 https://github.com/bblanchon/ArduinoJson.git .cache/ArduinoJson
  AJ=.cache/ArduinoJson
fi
python3 "$REPO/firmware/embed_files.py" >/dev/null   # generates src/embedded_files.h and src/build_info.h
mkdir -p bin out
# --wrap: tests set a fake clock without touching the machine's real one
g++ -std=gnu++17 -O0 -w -Wl,--wrap=time -Wl,--wrap=settimeofday -DARDUINO=10819 \
  -Istub -I"$AJ/src" -I"$REPO/firmware/src" "$1" stub/*.cpp \
  "$REPO/firmware/src/buddy.cpp" "$REPO"/firmware/src/buddies/*.cpp -o "bin/$2"
