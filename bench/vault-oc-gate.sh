#!/bin/bash
# B00tGeni3 Phase 7a: does OpenCore itself enforce the signed vault on the bench? No tool changes.
# Runs on: a macOS 12.7.6 build host, normal user. Boots bench/ESP-vault (signed with the THROWAWAY
# bench key) in QEMU. Only bench images are written; bench/ESP and the real EFI are not touched.
#
# Cases (each boots a fresh image):
#   good-release  signed set as built                         -> picker
#   good-debug    same, key-patched DEBUG OpenCore.efi        -> picker, no OCST errors
#   t1..t5        tampered sets placed from the host (DEBUG)  -> halt before the picker, OCST/OC line in serial
# Usage: vault-oc-gate.sh [case ...]   (default: all)

P7="$(cd "$(dirname "$0")" && pwd)"
BENCH="$P7/../bench"
ESPV="$BENCH/ESP-vault"
DBG="$P7/OpenCore-DEBUG-benchkey.efi"
TAMPER="$P7/tamper"
STAMP=$(date +%Y%m%d-%H%M%S)
REPORT="$BENCH/logs/$STAMP-vault-oc-gate.txt"
CASES="${*:-good-release good-debug t1 t2 t3 t4 t5}"

for f in "$ESPV/EFI/OC/OpenCore.efi" "$ESPV/EFI/OC/vault.plist" "$ESPV/EFI/OC/vault.sig" "$DBG"; do
  [ -f "$f" ] || { echo "[ERROR] missing $f"; exit 1; }
done

exec > >(tee "$REPORT") 2>&1
echo "[INFO] Phase 7a vault boot test - $(date)"
echo "[INFO] master $ESPV, RELEASE OpenCore.efi sha256 $(shasum -a 256 "$ESPV/EFI/OC/OpenCore.efi" | awk '{print $1}')"
echo "[INFO] DEBUG $DBG sha256 $(shasum -a 256 "$DBG" | awk '{print $1}')"

FAILS=0
# boot NAME EXPECT(picker|halt) PATTERN -- bench args...
boot() {
  local name="$1" expect="$2" pattern="$3"; shift 4
  echo
  echo "[INFO] case $name: expect $expect${pattern:+, serial must show: $pattern}"
  for try in 1 2; do
    out=$("$BENCH/bench.sh" --esp "$ESPV" --headless 30 "$@" < /dev/null)
    echo "$out" | grep -E '^\[(ERROR|WARN)\]|overlay in image|removed from image|OpenCore.efi in image'
    echo "$out" | grep -q "screenshot not written" || break
    echo "    [WARN] harness failure (monitor socket silent) - retrying this case once"
  done
  serial=$(echo "$out" | awk '/^\[INFO\] serial log:/{print $NF}')
  shot=$(echo "$out" | awk '/^\[INFO\] screenshot:/{print $NF}')
  [ -n "$shot" ] && cp "$shot" "$P7/shots/7a-$name.png"
  lines=$(grep -a -E 'OCST:|OCS:|OC: (Failed|Configuration requires|Invalid Vault)|OCB: Showing menu|OCUI: Load|OCB: Init pickers' "$serial" | head -8)
  echo "$lines" | sed 's/^/    serial: /'
  picker=0; grep -a -q 'OCB: Showing menu' "$serial" && picker=1
  ok=1
  if [ "$expect" = picker ]; then
    if [ "$name" = good-release ]; then
      # RELEASE prints no log: judge by the screenshot not being the black halt screen (size check)
      [ -n "$shot" ] && [ "$(stat -f %z "$shot")" -gt 20000 ] || ok=0
    else
      [ "$picker" -eq 1 ] || ok=0
      grep -a -q -E 'OCST: (Aborting|Invalid)|OCS: Missing' "$serial" && ok=0
    fi
  else
    [ "$picker" -eq 0 ] || ok=0
    grep -a -q -E "$pattern" "$serial" || ok=0
  fi
  if [ "$ok" -eq 1 ]; then echo "    case $name: PASS"; else echo "    [ERROR] case $name: FAIL"; FAILS=$((FAILS + 1)); fi
}

mkdir -p "$P7/shots"
for c in $CASES; do
  case "$c" in
    good-release) boot good-release picker "" -- ;;
    good-debug)   boot good-debug picker "" -- --debug --oc-debug "$DBG" ;;
    t1) boot t1-config-edited halt 'OCST: Aborting corrupted config.plist' -- --debug --oc-debug "$DBG" --overlay "$TAMPER/t1-config-edited" ;;
    t2) boot t2-wrong-key halt 'OCST: Invalid vault signature' -- --debug --oc-debug "$DBG" --overlay "$TAMPER/t2-wrong-key" ;;
    t3) boot t3-no-signature halt 'OCS: Missing vault signature' -- --debug --oc-debug "$DBG" --remove EFI/OC/vault.sig ;;
    t4) boot t4-file-not-in-vault halt 'OCST: Aborting .*OpenRuntime.efi file access not present in vault' -- --debug --oc-debug "$DBG" --overlay "$TAMPER/t4-file-not-in-vault" ;;
    t5) boot t5-no-vault-at-all halt 'OCS: Missing vault signature' -- --debug --oc-debug "$DBG" --remove EFI/OC/vault.sig --remove EFI/OC/vault.plist ;;
    *) echo "[ERROR] unknown case $c"; FAILS=$((FAILS + 1)) ;;
  esac
done
echo
echo "[INFO] report: $REPORT"
if [ "$FAILS" -eq 0 ]; then echo "[INFO] PHASE 7a VAULT BOOT TEST: PASS"; else echo "[ERROR] PHASE 7a VAULT BOOT TEST: $FAILS case(s) failed"; fi
[ "$FAILS" -eq 0 ]
