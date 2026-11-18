#!/bin/bash
# B00tGeni3 Phase 7b: the 0.8g tool with a signed vault, on the bench (QEMU + OVMF). Bench images only.
# Runs on: a macOS 12.7.6 build host, normal user. Needs phase7/make-tamper.sh run after the last re-sign.
#
#   refuse   tool lists the 4 tampered sets as REFUSED, shows each reason, COMMIT of an edited
#            boot-arg line is refused; config.plist / vault files unchanged afterwards
#   swap     known-good -> test1 -> known-good with the 3-file swap; OpenCore (key-patched DEBUG
#            build) must reach the picker after each reset with no OCST error; host check of all files
#   oc       the same 4 sets placed live: OpenCore must stop before the picker with the OCST/OCS line
#   gap      plain bench ESP (no key, Vault Optional) with only an OC\vault.plist: test1 has no set
#            and must be refused (0.8f would have swapped it and OpenCore would have stopped)
# Usage: vault-tool-gate.sh [part ...]   (default: refuse swap oc gap)

P7="$(cd "$(dirname "$0")" && pwd)"
BENCH="$(cd "$P7/../bench" && pwd)"
ESPV="$BENCH/ESP-vault"
VB="$ESPV/EFI/OC/B00tGeni3"
DBG="$P7/OpenCore-DEBUG-benchkey.efi"
IMG="$BENCH/run/esp.cdr"
MNT="$BENCH/run/vgate-mnt"
STAMP=$(date +%Y%m%d-%H%M%S)
REPORT="$BENCH/logs/$STAMP-vault-tool-gate.txt"
PARTS="${*:-refuse commit swap oc gap}"
DEV=""
SHOTS="$P7/shots"

detach() {
  [ -n "$DEV" ] || return 0
  diskutil unmount "$MNT" >/dev/null 2>&1
  hdiutil detach "$DEV" >/dev/null 2>&1 || echo "[WARN] could not detach $DEV - run: hdiutil detach $DEV"
  DEV=""
}
finish() {
  rc=$?
  detach; rmdir "$MNT" 2>/dev/null
  echo "[INFO] report: $REPORT"
  if [ "$rc" -eq 0 ]; then echo "[INFO] PHASE 7b VAULT TOOL TEST: PASS"; else echo "[ERROR] PHASE 7b VAULT TOOL TEST: FAIL (exit $rc)"; fi
  exit "$rc"
}
trap finish EXIT

sha() { shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'; }

# Attach the image read-only and record hashes of the files that matter, fsck and leftovers.
inspect() {
  local out part f
  out=$(hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" 2>&1) || { echo "[ERROR] attach failed: $out"; return 1; }
  DEV=$(echo "$out" | awk 'NR==1{print $1}')
  part=$(echo "$out" | awk '/Microsoft Basic Data/{print $1}')
  FSCK=clean; fsck_msdos -n "$part" >/dev/null 2>&1 || FSCK=PROBLEMS
  mkdir -p "$MNT"
  diskutil mount readOnly -mountPoint "$MNT" "$part" >/dev/null || { echo "[ERROR] read-only mount failed"; detach; return 1; }
  G_CFG=$(sha "$MNT/EFI/OC/config.plist"); G_VLT=$(sha "$MNT/EFI/OC/vault.plist"); G_SIG=$(sha "$MNT/EFI/OC/vault.sig")
  G_BCFG=$(sha "$MNT/EFI/OC/B00tGeni3/config.plist.bak"); G_BVLT=$(sha "$MNT/EFI/OC/B00tGeni3/vault.plist.bak")
  G_BSIG=$(sha "$MNT/EFI/OC/B00tGeni3/vault.sig.bak")
  G_ACT=$(head -1 "$MNT/EFI/OC/B00tGeni3/active.txt" 2>/dev/null)
  LEFT=""
  for f in EFI/OC/config.tmp EFI/OC/config.old EFI/OC/vault.tmp EFI/OC/vault.sig.tmp \
           EFI/OC/B00tGeni3/config.bak.tmp EFI/OC/B00tGeni3/vault.bak.tmp EFI/OC/B00tGeni3/vsig.bak.tmp EFI/OC/B00tGeni3/active.tmp; do
    [ -e "$MNT/$f" ] && LEFT="$LEFT $f"
  done
  detach
}

FAILS=0
check() { if eval "$2"; then echo "    ok   $1"; else echo "    [ERROR] FAIL $1"; FAILS=$((FAILS + 1)); fi; }

# bench ARGS... : run bench.sh, keep its output in OUT, copy every step screenshot to phase7/shots/7b-*
bench() {
  OUT=$("$BENCH/bench.sh" --headless 25 "$@" < /dev/null)
  echo "$OUT" | grep -E '^\[(ERROR|WARN)\]|overlay in image|removed from image|OpenCore.efi in image|finished'
  SERIAL=$(echo "$OUT" | awk '/^\[INFO\] serial log:/{print $NF}')
  for s in $(echo "$OUT" | awk '/step: screenshot/{print $NF}'); do
    [ -f "$s" ] && cp "$s" "$SHOTS/7b-${s##*-[0-9][0-9][0-9][0-9][0-9][0-9]-}"
  done
}

KG=$(sha "$VB/known-good.plist"); KGV=$(sha "$VB/known-good.vault"); KGS=$(sha "$VB/known-good.vsig")
T1=$(sha "$VB/test1.plist");      T1V=$(sha "$VB/test1.vault");      T1S=$(sha "$VB/test1.vsig")
for v in "$KG" "$KGV" "$KGS" "$T1" "$T1V" "$T1S"; do [ -n "$v" ] || { echo "[ERROR] vault bench sets missing in $VB"; exit 1; }; done
[ -d "$P7/tool-sets" ] && [ -d "$P7/oc-sets" ] || { echo "[ERROR] run phase7/make-tamper.sh first"; exit 1; }
mkdir -p "$SHOTS"

exec > >(tee "$REPORT") 2>&1
echo "[INFO] Phase 7b vault tool test - $(date)"
echo "[INFO] tool sha256 $(sha "$ESPV/EFI/OC/Tools/B00tGeni3.efi") (bench/ESP copy $(sha "$BENCH/ESP/EFI/OC/Tools/B00tGeni3.efi"))"
echo "[INFO] parts: $PARTS"

for part in $PARTS; do
  echo
  case "$part" in
  refuse)
    echo "[INFO] part refuse: 4 tampered sets in the variant folder, then COMMIT with an edited boot-arg line"
    # list order: known-good, s1-edited, s2-wrongkey, s3-notinvault, s4-unsigned, test1
    STEPS="key:ret,wait:15,shot:refuse-main"
    for s in s1 s2 s3 s4; do STEPS="$STEPS,key:down,wait:1,key:ret,wait:6,shot:refuse-$s,key:spc,wait:3"; done
    STEPS="$STEPS,key:down,wait:2,shot:refuse-test1-row"
    bench --esp "$ESPV" --overlay "$P7/tool-sets" --steps "$STEPS"
    inspect || exit 1
    check "fsck clean" '[ "$FSCK" = clean ]'
    check "config.plist still known-good" '[ "$G_CFG" = "$KG" ]'
    check "vault.plist still known-good's" '[ "$G_VLT" = "$KGV" ]'
    check "vault.sig still known-good's" '[ "$G_SIG" = "$KGS" ]'
    check "no backup written (nothing swapped)" '[ -z "$G_BCFG" ]'
    check "no leftovers" '[ -z "$LEFT" ]'
    ;;
  commit)
    echo "[INFO] part commit: edit the boot-arg line (toggle -v) and COMMIT while a vault is active"
    # The list cursor starts on known-good (top), so Up leaves the list: APPLY, field, args list, VERBOSE.
    # Enter toggles -v off; Down twice back to the field; Right = COMMIT.
    bench --esp "$ESPV" --steps "key:ret,wait:15,key:up,wait:1,key:up,wait:1,key:up,wait:1,key:up,wait:1,shot:commit-verbose-sel,key:ret,wait:1,key:down,wait:1,key:down,wait:1,key:right,wait:1,shot:commit-sel,key:ret,wait:8,shot:commit"
    inspect || exit 1
    check "fsck clean" '[ "$FSCK" = clean ]'
    check "config.plist still known-good" '[ "$G_CFG" = "$KG" ]'
    check "vault.plist / vault.sig unchanged" '[ "$G_VLT" = "$KGV" ] && [ "$G_SIG" = "$KGS" ]'
    check "no backup written (nothing saved)" '[ -z "$G_BCFG" ]'
    ;;
  swap)
    echo "[INFO] part swap: known-good -> test1 -> known-good, key-patched DEBUG OpenCore.efi so the serial log shows each boot"
    bench --esp "$ESPV" --debug --oc-debug "$DBG" \
      --steps "key:ret,wait:15,key:down,wait:1,key:ret,wait:8,shot:swap1-pre,key:down,wait:1,key:ret,wait:35,shot:swap1-picker,key:ret,wait:15,shot:swap1-tool"
    menus=$(grep -a -c 'OCB: Showing menu' "$SERIAL"); bad=$(grep -a -c -E 'OCST: (Aborting|Invalid)|OCS: Missing|OC: Failed' "$SERIAL")
    check "OpenCore reached the picker twice (before and after the reset): $menus" '[ "$menus" -ge 2 ]'
    check "no OCST/OCS/OC failure lines: $bad" '[ "$bad" -eq 0 ]'
    inspect || exit 1
    check "fsck clean" '[ "$FSCK" = clean ]'
    check "config.plist = test1" '[ "$G_CFG" = "$T1" ]'
    check "vault.plist = test1.vault" '[ "$G_VLT" = "$T1V" ]'
    check "vault.sig = test1.vsig" '[ "$G_SIG" = "$T1S" ]'
    check "backups = known-good set" '[ "$G_BCFG" = "$KG" ] && [ "$G_BVLT" = "$KGV" ] && [ "$G_BSIG" = "$KGS" ]'
    check "active.txt = test1.plist ($G_ACT)" '[ "$G_ACT" = test1.plist ]'
    check "no leftovers:${LEFT:- none}" '[ -z "$LEFT" ]'
    # back: known-good is first in the list
    bench --esp "$ESPV" --debug --oc-debug "$DBG" --keep-image \
      --steps "key:ret,wait:15,key:ret,wait:8,shot:swap2-pre,key:down,wait:1,key:ret,wait:35,shot:swap2-picker"
    menus=$(grep -a -c 'OCB: Showing menu' "$SERIAL"); bad=$(grep -a -c -E 'OCST: (Aborting|Invalid)|OCS: Missing|OC: Failed' "$SERIAL")
    check "OpenCore reached the picker twice: $menus" '[ "$menus" -ge 2 ]'
    check "no OCST/OCS/OC failure lines: $bad" '[ "$bad" -eq 0 ]'
    inspect || exit 1
    check "fsck clean" '[ "$FSCK" = clean ]'
    check "config.plist = known-good" '[ "$G_CFG" = "$KG" ]'
    check "vault.plist = known-good.vault" '[ "$G_VLT" = "$KGV" ]'
    check "vault.sig = known-good.vsig" '[ "$G_SIG" = "$KGS" ]'
    check "backups = test1 set" '[ "$G_BCFG" = "$T1" ] && [ "$G_BVLT" = "$T1V" ] && [ "$G_BSIG" = "$T1S" ]'
    check "active.txt = known-good.plist ($G_ACT)" '[ "$G_ACT" = known-good.plist ]'
    check "no leftovers:${LEFT:- none}" '[ -z "$LEFT" ]'
    ;;
  oc)
    echo "[INFO] part oc: the same sets placed live, OpenCore must stop before the picker"
    for s in s1-edited:'OCST: Aborting corrupted config.plist' s2-wrongkey:'OCST: Invalid vault signature' \
             s3-notinvault:'OCST: Aborting Drivers.OpenRuntime.efi file access not present in vault' \
             s4-unsigned:'OCST: Aborting corrupted config.plist'; do
      name=${s%%:*}; pat=${s#*:}
      echo "  [INFO] $name: expect '$pat'"
      bench --esp "$ESPV" --debug --oc-debug "$DBG" --overlay "$P7/oc-sets/$name" --steps "shot:oc-$name"
      grep -a -E 'OCST:|OCS:|OC: Failed' "$SERIAL" | head -3 | sed 's/^/      serial: /'
      check "$name stops OpenCore with '$pat'" "grep -a -q -E '$pat' '$SERIAL' && ! grep -a -q 'OCB: Showing menu' '$SERIAL'"
    done
    ;;
  gap)
    echo "[INFO] part gap: plain bench ESP (no key, Vault Optional) plus an OC\\vault.plist made for its config"
    G="$P7/gap-overlay/EFI/OC"; rm -rf "$P7/gap-overlay"; mkdir -p "$G" || exit 1
    python3 -I "$P7/tools/vault-sign.py" make "$BENCH/ESP/EFI/OC" "$BENCH/ESP/EFI/OC/config.plist" "$P7/bench-test-key-THROWAWAY" "$G/vault.plist" "$G/vault.sig.unused" | sed 's/^/    /'
    rm -f "$G/vault.sig.unused"
    # list on the plain bench: known-good, test1. test1 is second.
    bench --overlay "$P7/gap-overlay" --steps "key:ret,wait:15,shot:gap-main,key:down,wait:1,key:ret,wait:6,shot:gap-test1"
    inspect || exit 1
    check "config.plist unchanged (plain known-good)" '[ "$G_CFG" = "$(sha "$BENCH/ESP/EFI/OC/config.plist")" ]'
    check "nothing swapped (no backup written)" '[ -z "$G_BCFG" ]'
    ;;
  *) echo "[ERROR] unknown part $part"; FAILS=$((FAILS + 1)) ;;
  esac
done
echo
echo "[INFO] failed checks: $FAILS"
[ "$FAILS" -eq 0 ]
