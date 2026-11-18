#!/usr/bin/env python3
"""B00tGeni3 Mac-side vault helper (Phase 7). The tool on the ESP never signs; this does.

Runs on: macOS (12.7.6 tested), any user, python3 + /usr/bin/openssl (LibreSSL).
Writes only to the paths given on the command line. No /tmp.

Why not OpenCore's sign.command: RsaTool -sign makes a NEW RSA key on every run and throws the
private key away, so only one config can ever be signed for one OpenCore.efi. B00tGeni3 needs
several signed configs on one ESP, so this uses one persistent key (openssl) for all of them.
The vault.plist format follows OpenCore 1.0.8 Utilities/CreateVault/create_vault.sh.

Commands:
  keygen KEYDIR [RSATOOL]          new 2048-bit key: KEYDIR/vault-key.pem (private, mode 600),
                                   vault-pub.pem, vault-pub528.bin (OpenCore's key layout).
                                   With RSATOOL, the 528 bytes are cross-checked against `RsaTool -pub`.
  embed EFI_IN EFI_OUT PUB528      copy OpenCore.efi with the key written after =BEGIN OC VAULT=
  sign OCDIR KEYDIR                sign every variant in OCDIR/B00tGeni3/*.plist (-> NAME.vault,
                                   NAME.vsig next to it) and the live config (-> OCDIR/vault.plist,
                                   OCDIR/vault.sig). Run again after ANY change to a file under OC.
  make OCDIR CONFIG KEYDIR OUTV OUTS   one set: vault for OCDIR with CONFIG as config.plist
  check OCDIR KEYDIR               verify every set in OCDIR against the files on disk (host side)
"""
import base64
import hashlib
import os
import re
import subprocess
import sys

OPENSSL = "/usr/bin/openssl"
BEGIN = b"=BEGIN OC VAULT="
END = b"==END OC VAULT=="
KEY_SIZE = 528
TOOL_DIR = "B00tGeni3"          # never read by OpenCore; the tool writes backups there


def die(msg):
    print("[ERROR] " + msg)
    sys.exit(1)


def run(args, **kw):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)
    if r.returncode != 0:
        die("%s failed: %s" % (" ".join(args), r.stderr.decode(errors="replace").strip()))
    return r.stdout


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.digest()


# ---------------------------------------------------------------- key
def pub528_from_modulus(n):
    """OC_RSA_PUBLIC_KEY_2048 as RsaTool.c output() writes it: NumQwords, N0Inv, N, R^2 mod N (LE)."""
    nbytes = (n.bit_length() + 7) // 8
    if nbytes != 256:
        die("modulus is %d bits, need 2048" % n.bit_length())
    nwords = nbytes // 8
    n0inv = (-pow(n, -1, 1 << 64)) % (1 << 64)      # needs python 3.8+
    rr = pow(1 << n.bit_length(), 2, n)
    out = nwords.to_bytes(8, "little") + n0inv.to_bytes(8, "little")
    out += n.to_bytes(nbytes, "little") + rr.to_bytes(nbytes, "little")
    assert len(out) == KEY_SIZE
    return out


def modulus_of(pem, private):
    args = [OPENSSL, "rsa", "-in", pem, "-noout", "-modulus"]
    if not private:
        args.insert(2, "-pubin")
    m = re.search(rb"Modulus=([0-9A-Fa-f]+)", run(args))
    if not m:
        die("no modulus in %s" % pem)
    return int(m.group(1), 16)


def cmd_keygen(keydir, rsatool=None):
    os.makedirs(keydir, exist_ok=True)
    key = os.path.join(keydir, "vault-key.pem")
    if os.path.exists(key):
        die("%s exists - refusing to overwrite a key (move it away first)" % key)
    old = os.umask(0o077)
    try:
        run([OPENSSL, "genrsa", "-out", key, "2048"])
    finally:
        os.umask(old)
    os.chmod(key, 0o600)
    pub = os.path.join(keydir, "vault-pub.pem")
    run([OPENSSL, "rsa", "-in", key, "-pubout", "-out", pub])
    blob = pub528_from_modulus(modulus_of(key, True))
    with open(os.path.join(keydir, "vault-pub528.bin"), "wb") as f:
        f.write(blob)
    print("[INFO] key: %s (private, keep it off the ESP)" % key)
    print("[INFO] public: %s, vault-pub528.bin sha256 %s" % (pub, hashlib.sha256(blob).hexdigest()))
    if rsatool:
        text = run([rsatool, "-pub", pub]).decode()
        ref = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", text))
        if ref != blob:
            die("RsaTool -pub gives different key bytes (%d vs %d bytes)" % (len(ref), len(blob)))
        print("[INFO] cross-check: RsaTool -pub output == vault-pub528.bin (%d bytes)" % len(ref))


def find_key_slot(data):
    i = data.find(BEGIN)
    if i < 0 or data.find(BEGIN, i + 1) >= 0:
        die("=BEGIN OC VAULT= marker not found exactly once")
    if data[i + 16 + KEY_SIZE:i + 32 + KEY_SIZE] != END:
        die("==END OC VAULT== not where expected")
    return i + 16


def cmd_embed(efi_in, efi_out, pub528):
    data = bytearray(open(efi_in, "rb").read())
    blob = open(pub528, "rb").read()
    if len(blob) != KEY_SIZE:
        die("%s is %d bytes, need %d" % (pub528, len(blob), KEY_SIZE))
    off = find_key_slot(data)
    if any(data[off:off + KEY_SIZE]) and data[off:off + KEY_SIZE] != blob:
        die("%s already holds a different key" % efi_in)
    data[off:off + KEY_SIZE] = blob
    if os.path.abspath(efi_in) != os.path.abspath(efi_out) and os.path.exists(efi_out):
        die("%s exists - refusing to overwrite" % efi_out)
    with open(efi_out, "wb") as f:
        f.write(data)
    print("[INFO] %s: key at offset %d, sha256 %s" % (efi_out, off, hashlib.sha256(data).hexdigest()))


# ---------------------------------------------------------------- vault
def vault_files(ocdir):
    """Same selection as create_vault.sh, plus the B00tGeni3 folder left out (OpenCore never reads it)."""
    names = []
    for root, dirs, files in os.walk(ocdir):
        rel_root = os.path.relpath(root, ocdir)
        dirs[:] = [d for d in dirs if not d.startswith(".")
                   and not (rel_root == "." and d.lower() == TOOL_DIR.lower())]
        for fn in files:
            low = fn.lower()
            if fn.startswith(".") or low.startswith("vault.") or low == "memtest86.log" \
                    or (low.startswith("memtest86-report-") and low.endswith(".html")) or low == "opencore.efi":
                continue
            rel = os.path.normpath(os.path.join(rel_root, fn))
            names.append(rel)
    # create_vault.sh: LC_COLLATE=POSIX sort of "./path" lines == byte order of the paths
    names.sort(key=lambda s: ("./" + s).encode())
    return names


def xml_escape(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def build_vault(ocdir, config):
    names = vault_files(ocdir)
    if "config.plist" not in names:
        names.append("config.plist")
        names.sort(key=lambda s: ("./" + s).encode())
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">',
           '<plist version="1.0">', "<dict>", "\t<key>Files</key>", "\t<dict>"]
    for rel in names:
        path = config if rel == "config.plist" else os.path.join(ocdir, rel)
        digest = sha256_file(path)
        out += ["\t\t<key>%s</key>" % xml_escape(rel.replace("/", "\\")), "\t\t<data>",
                "\t\t" + base64.b64encode(digest).decode(), "\t\t</data>"]
    out += ["\t</dict>", "\t<key>Version</key>", "\t<integer>1</integer>", "</dict>", "</plist>", ""]
    return "\n".join(out).encode(), len(names)


def cmd_make(ocdir, config, keydir, outv, outs):
    data, count = build_vault(ocdir, config)
    with open(outv, "wb") as f:
        f.write(data)
    key = os.path.join(keydir, "vault-key.pem")
    run([OPENSSL, "dgst", "-sha256", "-sign", key, "-out", outs, outv])
    if os.path.getsize(outs) != 256:
        die("%s is %d bytes, need 256" % (outs, os.path.getsize(outs)))
    run([OPENSSL, "dgst", "-sha256", "-verify", os.path.join(keydir, "vault-pub.pem"), "-signature", outs, outv])
    print("[INFO] %s: %d files, signed -> %s" % (outv, count, os.path.basename(outs)))


def cmd_sign(ocdir, keydir):
    tooldir = os.path.join(ocdir, TOOL_DIR)
    variants = sorted(f for f in os.listdir(tooldir) if f.lower().endswith(".plist") and not f.startswith("."))
    if not variants:
        die("no variants in %s" % tooldir)
    for v in variants:
        stem = v[:-6]
        cmd_make(ocdir, os.path.join(tooldir, v), keydir,
                 os.path.join(tooldir, stem + ".vault"), os.path.join(tooldir, stem + ".vsig"))
    cmd_make(ocdir, os.path.join(ocdir, "config.plist"), keydir,
             os.path.join(ocdir, "vault.plist"), os.path.join(ocdir, "vault.sig"))


def parse_vault(path):
    text = open(path, "rb").read().decode()
    pairs = re.findall(r"<key>([^<]+)</key>\s*<data>\s*([A-Za-z0-9+/=\s]+?)\s*</data>", text)
    return {k.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">"): base64.b64decode(v) for k, v in pairs}


def cmd_check(ocdir, keydir):
    pub = os.path.join(keydir, "vault-pub.pem")
    sets = [("live config", os.path.join(ocdir, "config.plist"), os.path.join(ocdir, "vault.plist"),
             os.path.join(ocdir, "vault.sig"))]
    tooldir = os.path.join(ocdir, TOOL_DIR)
    for v in sorted(os.listdir(tooldir)):
        if v.lower().endswith(".plist"):
            stem = os.path.join(tooldir, v[:-6])
            sets.append((v, os.path.join(tooldir, v), stem + ".vault", stem + ".vsig"))
    bad = 0
    for name, cfg, vlt, sig in sets:
        problems = []
        if not os.path.exists(vlt) or not os.path.exists(sig):
            problems.append("no vault set")
        else:
            r = subprocess.run([OPENSSL, "dgst", "-sha256", "-verify", pub, "-signature", sig, vlt],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if r.returncode != 0:
                problems.append("signature does not verify")
            entries = parse_vault(vlt)
            if entries.get("config.plist") != sha256_file(cfg):
                problems.append("config.plist hash differs")
            for rel, digest in entries.items():
                if rel == "config.plist" or rel.split("\\")[0].lower() == TOOL_DIR.lower():
                    continue
                p = os.path.join(ocdir, *rel.split("\\"))
                if not os.path.exists(p):
                    problems.append("missing " + rel)
                elif sha256_file(p) != digest:
                    problems.append("changed " + rel)
            for rel in vault_files(ocdir):
                if rel.replace("/", "\\") not in entries:
                    problems.append("not in vault " + rel)
        print("  %-22s %s" % (name, "OK" if not problems else "BAD: " + "; ".join(problems)))
        bad += bool(problems)
    sys.exit(1 if bad else 0)


def main(a):
    cmds = {"keygen": (cmd_keygen, (1, 2)), "embed": (cmd_embed, (3, 3)), "sign": (cmd_sign, (2, 2)),
            "make": (cmd_make, (5, 5)), "check": (cmd_check, (2, 2))}
    if len(a) < 1 or a[0] not in cmds or not (cmds[a[0]][1][0] <= len(a) - 1 <= cmds[a[0]][1][1]):
        print(__doc__)
        sys.exit(2)
    cmds[a[0]][0](*a[1:])


if __name__ == "__main__":
    main(sys.argv[1:])
