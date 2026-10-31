#!/usr/bin/env bash
# Builds and runs every host-side test. Exit status 1 if any fails.
set -uo pipefail
cd "$(dirname "$0")"
fail=0
mkdir -p out && python3 ../py/gen_bridge_fixture.py out/bridge_state.json >/dev/null || { echo "FAIL host/bridge fixture"; fail=1; }
for t in update_test time_test rssi_test usage_test fold_test bridge_test power_test; do
  ./build.sh "$t.cpp" "$t" || { echo "FAIL build $t"; fail=1; continue; }
  out=$(./bin/$t 2>&1); rc=$?
  p=$(printf '%s\n' "$out" | grep -c '^PASS'); f=$(printf '%s\n' "$out" | grep -c '^FAIL')
  if [ $rc -ne 0 ] || [ "$f" -gt 0 ]; then echo "FAIL host/$t ($p pass, $f fail)"; printf '%s\n' "$out" | grep -v '^PASS' | tail -15; fail=1
  else echo "ok   host/$t ($p checks)"; fi
done
# Improv Wi-Fi serial protocol: a Python client drives the firmware's parser through the host build
./build.sh improv_test.cpp improv_test && (cd bin && cp ../improv_client.py . 2>/dev/null; true)
if python3 improv_client.py ./bin/improv_test >/tmp/improv.out 2>&1 && ! grep -q Traceback /tmp/improv.out; then echo "ok   host/improv ($(grep -c '^[0-9]' /tmp/improv.out) cases)"; else echo "FAIL host/improv"; tail -10 /tmp/improv.out; fail=1; fi
# Render every screen to PPM files in out/ (used for docs/screens.png); fails if the renderer crashes.
./build.sh render_screens.cpp render_screens && ./bin/render_screens >/dev/null 2>&1 && echo "ok   host/render ($(ls out | wc -l) frames)" || { echo "FAIL host/render"; fail=1; }
exit $fail
