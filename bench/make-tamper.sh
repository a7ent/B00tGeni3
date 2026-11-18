#!/bin/bash
# B00tGeni3 Phase 7: build the tampered vault sets from the CURRENT bench/ESP-vault, twice each:
#   phase7/tool-sets/EFI/OC/B00tGeni3/NAME.{plist,vault,vsig}  - as variants, for the tool to refuse
#   phase7/oc-sets/NAME/EFI/OC/{config.plist,vault.plist,vault.sig} - placed live, for OpenCore to refuse
# Same bytes in both places. Run after every re-sign of bench/ESP-vault.
# Runs on: macOS build host. Writes only under phase7/.
#   s1-edited     known-good with one boot-arg added after signing, known-good's vault + sig
#   s2-wrongkey   test1 with test1's vault, signature from a second throwaway key
#   s3-notinvault test1 with a vault missing Drivers\OpenRuntime.efi, signed with the bench key
#   s4-unsigned   test1 with no set (OpenCore side: test1 config under known-good's vault)
set -u
P7="$(cd "$(dirname "$0")" && pwd)"
V="$P7/../bench/ESP-vault/EFI/OC"
B="$V/B00tGeni3"
KEY="$P7/bench-test-key-THROWAWAY/vault-key.pem"
OTHER="$P7/bench-test-key-OTHER-THROWAWAY/vault-key.pem"
GUID=7C436110-AB2A-4BBB-A880-FE41995C9F82
TS="$P7/tool-sets/EFI/OC/B00tGeni3"
OS="$P7/oc-sets"
for f in "$KEY" "$OTHER" "$B/known-good.vault" "$B/test1.vault" "$B/test1.vsig"; do
  [ -f "$f" ] || { echo "[ERROR] missing $f"; exit 1; }
done
rm -rf "$P7/tool-sets" "$OS"
mkdir -p "$TS" || exit 1
for s in s1-edited s2-wrongkey s3-notinvault s4-unsigned; do mkdir -p "$OS/$s/EFI/OC" || exit 1; done

# s1
cp "$B/known-good.plist" "$TS/s1-edited.plist"
cur=$(/usr/libexec/PlistBuddy -c "Print :NVRAM:Add:$GUID:boot-args" "$TS/s1-edited.plist") || exit 1
/usr/libexec/PlistBuddy -c "Set :NVRAM:Add:$GUID:boot-args '$cur debug=0x100'" "$TS/s1-edited.plist" || exit 1
cp "$B/known-good.vault" "$TS/s1-edited.vault"; cp "$B/known-good.vsig" "$TS/s1-edited.vsig"
# s2
cp "$B/test1.plist" "$TS/s2-wrongkey.plist"; cp "$B/test1.vault" "$TS/s2-wrongkey.vault"
/usr/bin/openssl dgst -sha256 -sign "$OTHER" -out "$TS/s2-wrongkey.vsig" "$TS/s2-wrongkey.vault" || exit 1
# s3
cp "$B/test1.plist" "$TS/s3-notinvault.plist"
python3 -I - "$B/test1.vault" "$TS/s3-notinvault.vault" <<'EOF' || exit 1
import re, sys
t = open(sys.argv[1]).read()
n = re.sub(r'\t\t<key>Drivers\\OpenRuntime\.efi</key>\n\t\t<data>\n\t\t[^\n]*\n\t\t</data>\n', '', t)
assert n != t, "OpenRuntime entry not found"
open(sys.argv[2], 'w').write(n)
EOF
/usr/bin/openssl dgst -sha256 -sign "$KEY" -out "$TS/s3-notinvault.vsig" "$TS/s3-notinvault.vault" || exit 1
# s4
cp "$B/test1.plist" "$TS/s4-unsigned.plist"

# OpenCore side: the same bytes as the live files
cp "$TS/s1-edited.plist" "$OS/s1-edited/EFI/OC/config.plist"
cp "$TS/s1-edited.vault" "$OS/s1-edited/EFI/OC/vault.plist"; cp "$TS/s1-edited.vsig" "$OS/s1-edited/EFI/OC/vault.sig"
for s in s2-wrongkey s3-notinvault; do
  cp "$TS/$s.plist" "$OS/$s/EFI/OC/config.plist"
  cp "$TS/$s.vault" "$OS/$s/EFI/OC/vault.plist"; cp "$TS/$s.vsig" "$OS/$s/EFI/OC/vault.sig"
done
cp "$TS/s4-unsigned.plist" "$OS/s4-unsigned/EFI/OC/config.plist"
echo "[INFO] tool sets: $(cd "$TS" && ls | tr '\n' ' ')"
echo "[INFO] oc sets:   $(cd "$OS" && ls | tr '\n' ' ')"
