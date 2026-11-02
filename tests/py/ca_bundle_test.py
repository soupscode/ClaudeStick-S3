"""The Stick's embedded CA bundle: well-formed, sorted, and holding the roots that real servers' chains end at."""
import os
import subprocess
import sys
import warnings

warnings.filterwarnings("ignore")
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, ROOT + "/tools")
try:
    import ca_bundle as cb  # noqa: E402
except ImportError:
    print("SKIP the CA-bundle test needs `pip install cryptography`")
    sys.exit(0)

FAILED = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (("  | " + str(extra)[:200]) if extra and not cond else ""))
    if not cond:
        FAILED.append(name)


data = open(cb.BUNDLE, "rb").read()
ents = cb.parse(data)
names = [cb.name_of(n) for n, _ in ents]
check("the bundle parses to its declared count with no stray bytes", len(ents) > 100)
check("it round-trips byte for byte (so it is sorted by subject, as the verifier's binary search needs)", cb.build(ents) == data)


def has(s):
    return any(s in n for n in names)


for want in ("ISRG Root X1", "ISRG Root X2", "DigiCert Global Root G2", "GTS Root R1", "GTS Root R4", "Amazon Root CA 1",
             "USERTrust RSA Certification Authority", "GlobalSign Root CA - R3"):
    check("trusts " + want, has(want))
check("trusts the GlobalSign Root CA (R1) that Google Trust Services cross-signs its roots with (Cloudflare's chain)",
      any(n.endswith("GlobalSign Root CA") for n in names))
extra = cb.load_pem(cb.EXTRA)
check("every root in extra_roots.pem is in the bundle", extra and all(cb.entry(c) in ents for c in extra))
import datetime
now = datetime.datetime.now(datetime.timezone.utc)
check("no extra root has expired", all(c.not_valid_after_utc > now for c in extra), [c.subject.rfc4514_string() for c in extra])
soon = [c.subject.rfc4514_string() for c in extra if c.not_valid_after_utc < now + datetime.timedelta(days=120)]
if soon:
    print("NOTE extra root(s) expire within 120 days, time to drop them:", soon)
r = subprocess.run([sys.executable, "-W", "ignore", ROOT + "/tools/ca_bundle.py", "--check"], capture_output=True, text=True)
check("`ca_bundle.py --check` is happy", r.returncode == 0, r.stdout + r.stderr)

print("\nca bundle: %d failed" % len(FAILED) if FAILED else "\nca bundle: all passed")
sys.exit(1 if FAILED else 0)
