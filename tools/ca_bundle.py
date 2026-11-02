#!/usr/bin/env python3
"""Inspect and extend the Stick's embedded CA bundle (firmware/data/cert/x509_crt_bundle.bin).

The file is the ESP-IDF certificate-bundle format: a big-endian u16 count, then per root
[u16 subject length][u16 public-key length][subject DER][SubjectPublicKeyInfo DER], sorted by subject
(the verifier binary-searches it). tools/gen_ca_bundle.sh builds it from Mozilla's roots (certifi); this tool
adds the roots listed in firmware/data/cert/extra_roots.pem to an existing bundle without needing PlatformIO.

  python3 tools/ca_bundle.py --list            what is in the bundle
  python3 tools/ca_bundle.py --add             add the roots from firmware/data/cert/extra_roots.pem (idempotent)
  python3 tools/ca_bundle.py --check           exit 1 if the bundle is malformed, unsorted or lacks an extra root
Needs: pip install cryptography
"""
import argparse
import datetime
import os
import re
import struct
import sys

from cryptography import x509
from cryptography.hazmat.primitives import serialization as ser

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BUNDLE = os.path.join(ROOT, "firmware", "data", "cert", "x509_crt_bundle.bin")
EXTRA = os.path.join(ROOT, "firmware", "data", "cert", "extra_roots.pem")


def parse(data):
    """-> list of (subject DER, SPKI DER)."""
    count = struct.unpack(">H", data[:2])[0]
    pos, out = 2, []
    for _ in range(count):
        nl, kl = struct.unpack(">HH", data[pos:pos + 4])
        out.append((data[pos + 4:pos + 4 + nl], data[pos + 4 + nl:pos + 4 + nl + kl]))
        pos += 4 + nl + kl
    if pos != len(data):
        raise ValueError(f"{len(data) - pos} stray bytes after {count} entries")
    return out


def build(entries):
    entries = sorted(set(entries), key=lambda e: e[0])
    return struct.pack(">H", len(entries)) + b"".join(struct.pack(">HH", len(n), len(k)) + n + k for n, k in entries)


def load_pem(path):
    """-> list of x509 certificates from a PEM file (comments allowed)."""
    text = open(path).read()
    certs = []
    for m in re.finditer(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", text, re.S):
        certs.append(x509.load_pem_x509_certificate(m.group(0).encode()))
    return certs


def entry(cert):
    return (cert.subject.public_bytes(), cert.public_key().public_bytes(ser.Encoding.DER, ser.PublicFormat.SubjectPublicKeyInfo))


def name_of(subject_der):
    m = re.findall(rb"[\x20-\x7e]{4,}", subject_der)
    return " / ".join(s.decode("latin1") for s in m)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--list", action="store_true")
    p.add_argument("--add", action="store_true")
    p.add_argument("--check", action="store_true")
    p.add_argument("--bundle", default=BUNDLE)
    a = p.parse_args()
    data = open(a.bundle, "rb").read()
    ents = parse(data)
    if a.list:
        for n, _ in ents:
            print(name_of(n))
        print(f"{len(ents)} roots")
    if a.add:
        extra = [entry(c) for c in load_pem(EXTRA)]
        new = build(ents + extra)
        if new == build(ents) and len(parse(new)) == len(ents):
            print(f"nothing to add ({len(ents)} roots)")
        else:
            open(a.bundle, "wb").write(new)
            print(f"{len(ents)} -> {len(parse(new))} roots, wrote {a.bundle}")
    if a.check:
        bad = []
        if build(ents) != data:
            bad.append("not sorted by subject, or has duplicates")
        have = {e[0] for e in ents}
        now = datetime.datetime.now(datetime.timezone.utc)
        for c in load_pem(EXTRA):
            if c.subject.public_bytes() not in have:
                bad.append("missing extra root: " + c.subject.rfc4514_string())
            if c.not_valid_after_utc < now:
                bad.append("extra root has expired: " + c.subject.rfc4514_string())
        for b in bad:
            print("FAIL", b)
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
