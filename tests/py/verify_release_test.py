"""tools/verify_release.py must pass a good release and fail every kind of broken one."""
import hashlib, json, os, shutil, struct, subprocess, sys, tempfile
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SCRIPT = os.path.join(ROOT, "tools", "verify_release.py")
URL = "https://owner.github.io/repo/"
fails = 0


def check(name, ok, extra=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else " | " + str(extra)[:200]))
    fails += 0 if ok else 1


def make(root, ver="0.8.0"):
    os.makedirs(root + "/firmware/src"); os.makedirs(root + "/web/firmware")
    open(root + "/firmware/src/main.cpp", "w").write('static const char* FW_VERSION   = "%s";\n' % ver)
    app = b"\xe9" + os.urandom(1000) + ver.encode() + b"\0" + URL.encode() + b"\0" + os.urandom(500)
    open(root + "/web/firmware/claudestick-app.bin", "wb").write(app)

    def ent(t, st, o, sz, label):
        return b"\xaa\x50" + struct.pack("<BBII", t, st, o, sz) + label.ljust(16, b"\0") + struct.pack("<I", 0)
    pt = ent(1, 2, 0x9000, 0x5000, b"nvs") + ent(1, 0, 0xe000, 0x2000, b"otadata") + \
        ent(0, 0x10, 0x10000, 0x330000, b"app0") + ent(0, 0x11, 0x340000, 0x330000, b"app1")
    img = bytearray(b"\xff" * 0x10000); img[0] = 0xe9; img[0x8000:0x8000 + len(pt)] = pt
    open(root + "/web/firmware/claudestick.bin", "wb").write(bytes(img) + app)
    json.dump({"version": ver, "app": "firmware/claudestick-app.bin", "sha256": hashlib.sha256(app).hexdigest(), "notes": "* new"}, open(root + "/web/ota.json", "w"))
    json.dump({"name": "x", "version": ver, "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": "firmware/claudestick.bin", "offset": 0}]}]}, open(root + "/web/manifest.json", "w"))


def run(root, url=URL):
    return subprocess.run([sys.executable, SCRIPT, "--root", root], capture_output=True, text=True, env={**os.environ, "CS_UPDATE_URL": url})


def edit_json(root, f, fn):
    p = root + "/web/" + f; x = json.load(open(p)); fn(x); json.dump(x, open(p, "w"))


base = tempfile.mkdtemp(); make(base + "/good")
r = run(base + "/good"); check("a good release passes", r.returncode == 0, r.stdout)
cases = {
    "version mismatch": lambda d: edit_json(d, "ota.json", lambda x: x.update(version="0.7.0")),
    "wrong sha256": lambda d: edit_json(d, "ota.json", lambda x: x.update(sha256="0" * 64)),
    "absolute app url": lambda d: edit_json(d, "ota.json", lambda x: x.update(app="https://evil/x.bin")),
    "manifest version": lambda d: edit_json(d, "manifest.json", lambda x: x.update(version="0.1.0")),
}
for name, fn in cases.items():
    d = base + "/c_" + name.replace(" ", "_"); shutil.copytree(base + "/good", d); fn(d)
    check("fails: " + name, run(d).returncode == 1)
d = base + "/c_bridge"; shutil.copytree(base + "/good", d); os.makedirs(d + "/bridge")
open(d + "/bridge/claudestick_bridge.py", "w").write('VERSION = "0.8.0"\n')
check("a bridge with the matching version passes", run(d).returncode == 0, run(d).stdout)
open(d + "/bridge/claudestick_bridge.py", "w").write('VERSION = "0.7.0"\n')
check("fails: bridge version differs from the firmware's", run(d).returncode == 1)
check("fails: update URL not baked in", run(base + "/good", "https://other.github.io/x/").returncode == 1)
d = base + "/c_slot"; shutil.copytree(base + "/good", d)
p = d + "/web/firmware/claudestick.bin"; b = bytearray(open(p, "rb").read()); b[0x8000 + 32 * 2 + 3] = 0x20; open(p, "wb").write(b)
check("fails: no OTA slot in the partition table", run(d).returncode == 1)
shutil.rmtree(base, ignore_errors=True)
sys.exit(1 if fails else 0)
