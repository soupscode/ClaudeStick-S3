#!/usr/bin/env python3
"""Sanity-check a packaged release (web/) before it is published, so an over-the-air update can't ship broken.

Checks that ota.json, manifest.json and the firmware images agree with each other and with the source:
  * version in ota.json / manifest.json == FW_VERSION in firmware/src/main.cpp
  * the app image exists, its SHA-256 matches ota.json, and it fits the OTA slot (the partition table
    in the merged image must have two OTA slots plus otadata, or updates can't work at all)
  * the merged image looks like a flash dump (bootloader at 0x0, partition table at 0x8000, app at 0x10000)
    and carries the same app image
  * the version string, and the baked-in update URL (CS_UPDATE_URL env), are inside the app image
  * the bridge's VERSION (bridge/claudestick_bridge.py), when present, equals FW_VERSION
  * the telemetry collector's VERSION (telemetry/claudestick_telemetry.py), when present, equals FW_VERSION
  * the update notes for this version aren't empty (warning only)

Usage: tools/verify_release.py [--root DIR]      Exit status 1 on any error. Prints GitHub annotations.
"""
import argparse, hashlib, json, os, re, struct, sys

ap = argparse.ArgumentParser()
ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
root = os.path.normpath(ap.parse_args().root)
errors, warnings, info = [], [], []


def err(m): errors.append(m)
def warn(m): warnings.append(m)
def path(*p): return os.path.join(root, *p)


def read(p, mode='rb'):
    try:
        with open(p, mode) as f:
            return f.read()
    except OSError as e:
        err(f'cannot read {os.path.relpath(p, root)}: {e.strerror}')
        return None


src = read(path('firmware', 'src', 'main.cpp'), 'r')
m = re.search(r'FW_VERSION\s*=\s*"([^"]+)"', src or '')
version = m.group(1) if m else None
if not version:
    err('FW_VERSION not found in firmware/src/main.cpp')

# the bridge ships in the same release: its version must match the firmware's
bridge_src = path('bridge', 'claudestick_bridge.py')
if os.path.exists(bridge_src):
    bm = re.search(r'^VERSION\s*=\s*"([^"]+)"', read(bridge_src, 'r') or '', re.M)
    if not bm or bm.group(1) != version:
        err(f'bridge VERSION {bm.group(1) if bm else None!r} != FW_VERSION {version!r}')

# ditto the telemetry collector
tel_src = path('telemetry', 'claudestick_telemetry.py')
if os.path.exists(tel_src):
    tm = re.search(r'^VERSION\s*=\s*"([^"]+)"', read(tel_src, 'r') or '', re.M)
    if not tm or tm.group(1) != version:
        err(f'telemetry collector VERSION {tm.group(1) if tm else None!r} != FW_VERSION {version!r}')

ota = None
raw = read(path('web', 'ota.json'), 'r')
if raw:
    try:
        ota = json.loads(raw)
    except ValueError as e:
        err(f'web/ota.json is not valid JSON: {e}')

app = None
slot_size = None
if ota:
    if ota.get('version') != version:
        err(f'ota.json version {ota.get("version")!r} != FW_VERSION {version!r}')
    rel = ota.get('app', '')
    if not rel or rel.startswith('/') or '://' in rel or '..' in rel:
        err(f'ota.json "app" must be a relative path, got {rel!r}')
    else:
        app = read(path('web', rel))
        if app is not None:
            sha = hashlib.sha256(app).hexdigest()
            if ota.get('sha256') != sha:
                err('ota.json sha256 does not match the app image')
    if not str(ota.get('notes', '')).strip():
        warn(f'no changelog notes for {version} in CHANGELOG.md: the update prompt will say "(no changelog)"')

man = read(path('web', 'manifest.json'), 'r')
if man:
    try:
        mj = json.loads(man)
        if mj.get('version') != version:
            err(f'manifest.json version {mj.get("version")!r} != FW_VERSION {version!r}')
        parts = (mj.get('builds') or [{}])[0].get('parts') or []
        if not parts or parts[0].get('offset') != 0:
            err('manifest.json must flash one merged image at offset 0')
        else:
            merged_path = path('web', parts[0].get('path', ''))
            merged = read(merged_path)
            if merged is not None:
                if len(merged) > 8 * 1024 * 1024:
                    err('merged image is larger than the 8 MB flash')
                if merged[:1] != b'\xe9':
                    err('merged image does not start with a bootloader (0xE9)')
                if merged[0x8000:0x8002] != b'\xaa\x50':
                    err('no partition table at 0x8000 in the merged image')
                else:
                    ents = {}
                    off = 0x8000
                    while merged[off:off + 2] == b'\xaa\x50':
                        t, st, o, sz = struct.unpack_from('<BBII', merged, off + 2)
                        label = merged[off + 12:off + 28].split(b'\0')[0].decode('ascii', 'replace')
                        ents[(t, st)] = (label, o, sz)
                        off += 32
                    if (0, 0x10) not in ents or (0, 0x11) not in ents:
                        err('partition table has no ota_0/ota_1 slots: over-the-air updates cannot work')
                    else:
                        slot_size = ents[(0, 0x10)][2]
                        if ents[(0, 0x11)][2] < slot_size:
                            warn('ota_1 is smaller than ota_0')
                    if (1, 0x00) not in ents:
                        err('partition table has no otadata partition')
                if merged[0x10000:0x10001] != b'\xe9':
                    err('merged image has no app at 0x10000')
                elif app is not None and merged[0x10000:0x10000 + len(app)] != app:
                    err('the app inside the merged image differs from claudestick-app.bin')
    except ValueError as e:
        err(f'web/manifest.json is not valid JSON: {e}')

if app is not None:
    if app[:1] != b'\xe9':
        err('app image does not start with 0xE9')
    if slot_size:
        pct = 100.0 * len(app) / slot_size
        info.append(f'app {len(app) / 1024:.0f} KB = {pct:.0f}% of the {slot_size // 1024} KB OTA slot')
        if len(app) > slot_size:
            err('app image does not fit the OTA slot')
        elif pct > 90:
            warn(f'app uses {pct:.0f}% of the OTA slot: little headroom left')
    else:
        info.append(f'app {len(app) / 1024:.0f} KB')
    if version and version.encode() not in app:
        err(f'version string {version!r} not found inside the app image')
    url = os.environ.get('CS_UPDATE_URL', '')
    if url and url.encode() not in app:
        err(f'update URL {url!r} was not baked into the app image: the Stick could not update itself')
    elif url:
        info.append(f'update source: {url}')

for w in warnings:
    print(f'::warning title=release::{w}')
for e in errors:
    print(f'::error title=release check failed::{e}')
if errors:
    sys.exit(1)
print('::notice title=release::' + ('; '.join(info) if info else 'ok') + (f'; {len(warnings)} warning(s)' if warnings else ''))
print('release checks passed' + (' (' + '; '.join(info) + ')' if info else ''))
