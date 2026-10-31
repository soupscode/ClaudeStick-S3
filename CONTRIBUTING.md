# Contributing

Thanks for helping. Bug reports with the firmware version (menu → info), the "last reset" line and what you did are the most useful thing you can send.

## Set up

```bash
pip install platformio "esptool<5"        # to build the firmware
./tools/build_web.sh                      # build + package the web flasher into web/
python3 -m http.server -d web 8000        # open http://localhost:8000 in Chrome or Edge
python3 tools/mock_device.py --token test123   # the Stick's API, phone page and wizard, no hardware needed
python3 bridge/claudestick_bridge.py --data /tmp/bridge   # the bridge (see docs/REMOTE.md)
```

`firmware/src/embedded_files.h` and `build_info.h` are generated at build time (`firmware/embed_files.py`); don't commit them.

## Tests

```bash
tests/run_all.sh                 # everything below
tests/run_all.sh --no-browser    # skip headless Chromium (pip install playwright && playwright install chromium)
tests/run_all.sh --no-host       # skip the C++ firmware tests (needs g++ and git)
```

- `tests/py`: the hook, installer, status-line forwarding and self-update against `tools/mock_device.py`, `tools/verify_release.py`, and the bridge (`e2e_bridge.py`: auth, limits, persistence, TLS; `e2e_bridge_hooks.py`: the real hook against it; `contract_bridge.py`: the bridge answers hooks exactly like the Stick's firmware, as modelled by the mock).
- `tests/host`: the real firmware sources compiled on your machine against small Arduino/M5/ESP stand-ins (`tests/host/stub`). They cover update logic, the clock and time zones, Wi-Fi bars, usage data, text folding, the Improv serial protocol, the bridge client and multiple Wi-Fi networks (the bridge's replies are scripted), and they render every screen to `tests/host/out/*.ppm`.
- `tests/browser`: the setup wizard (Stick and bridge flavours) and phone page in headless Chromium.

CI runs all of this on every push and pull request; publishing is gated on the build, the release checks and the tests.

## Making a change

A change is ready when:

1. `tests/run_all.sh` passes, with new tests for new behavior (and the mock device updated if the API changed).
2. User-visible behavior is in `README.md`; design or API changes are in `docs/HOW_IT_WORKS.md`.
3. `CHANGELOG.md` has an entry. If the firmware changed, bump `FW_VERSION` in `firmware/src/main.cpp` (the bridge's `VERSION` in `bridge/claudestick_bridge.py` must match; a test checks) and add a `## x.y.z` section: its bullets are what the Stick's update prompt shows, so keep them short and plain.
4. Nothing personal or environment-specific is hard-coded: no names, hostnames, addresses, tokens or accounts in code, docs, tests or screenshots. Use neutral sample data (`api-server`, `home-wifi`).
5. Screenshots in `docs/` are regenerated if the UI changed (render with `tests/host/run.sh`, then `cd tests/host/out && python3 ../sheet.py ../../../docs/screens.png <frame names>`).
6. Generated assets are regenerated from their scripts, not edited by hand: `tools/gen_font.py` (text style), `tools/gen_tinyfont.py`, `tools/gen_ca_bundle.sh`.

## Releasing

Bump the version and changelog as above and push to `main`. CI builds, runs `tools/verify_release.py` and the tests, then publishes the flasher and `ota.json` to GitHub Pages (Settings → Pages → Source: GitHub Actions). Every Stick then offers the update. The build also reports firmware size and compiler warnings as annotations on the run; keep the app under about 90% of the OTA slot.

## Code notes

- Firmware text is ASCII only (6x8 glyphs); fold anything displayed. Layout constants and the corner/pet layouts are at the top of the UI section in `main.cpp`.
- Don't block the main loop: long operations (TLS, uploads) are guarded by idle and memory checks. Anything that could hold up an approval needs a reason.
- `firmware/src/ble_bridge.*` and `firmware/src/buddies/*` track upstream (anthropics/claude-desktop-buddy); avoid editing them.
