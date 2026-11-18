#!/bin/bash
# B00tGeni3 Phase 5 gate: deliberately broken variants must each be refused with a reason on screen.
# Runs on: a macOS 12.7.6 build host, normal user.
# Two QEMU sessions, each on a fresh image built from bench/ESP plus one batch of broken variants
# (bench.sh --extra-variants; the master ESP folder is never changed):
#   batch a: bad-acpi, bad-driver, bad-kext, bad-tool      then nothing else
#   batch b: bad-nopicker, bad-notool, bad-parse, bad-vault then test1.plist as the positive control
# In each session every broken variant is picked (menu keys 2-5) and a screenshot is taken of the
# refusal. After the session the image is attached READ-ONLY and checked from the host:
#   batch a: config.plist still == known-good, no backup, no active.txt, no temp files
#   batch b: config.plist == test1, backup == known-good, active.txt == test1.plist
# The refusal text itself is only on the screenshots: read them.
#
# Usage: phase5-gate.sh

BENCH="$(cd "$(dirname "$0")" && pwd)"
IMG="$BENCH/run/esp.cdr"
MNT="$BENCH/run/gate-mnt"
VAR="$BENCH/ESP/EFI/OC/B00tGeni3"
BROKEN="$BENCH/phase5-variants"
STAMP=$(date +%Y%m%d-%H%M%S)
REPORT="$BENCH/logs/$STAMP-phase5-gate.txt"
DEV=""
DONE=0

detach() {
  [ -n "$DEV" ] || return 0
  diskutil unmount "$MNT" >/dev/null 2>&1
  hdiutil detach "$DEV" >/dev/null 2>&1 || echo "[WARN] could not detach $DEV - run: hdiutil detach $DEV"
  DEV=""
}

finish() {
  rc=$?
  detach
  rmdir "$MNT" 2>/dev/null
  echo "[INFO] report: $REPORT"
  if [ "$DONE" -ne 1 ]; then
    echo "[ERROR] PHASE 5 GATE: INCOMPLETE (stopped before the end, exit $rc)"
    [ "$rc" -ne 0 ] || rc=1
  elif [ "$rc" -eq 0 ]; then
    echo "[INFO] PHASE 5 GATE: host checks PASS - now read the refusal screenshots"
  else
    echo "[ERROR] PHASE 5 GATE: FAIL (exit $rc)"
  fi
  exit "$rc"
}
trap finish EXIT

sha() { shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'; }

# check <expected config sha> <expected backup sha or NONE> <expected active name or NONE>
check() {
  local want_cfg="$1" want_bak="$2" want_name="$3" out part got_cfg got_bak got_name ok=0
  out=$(hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" 2>&1) || { echo "[ERROR] attach failed: $out"; return 1; }
  DEV=$(echo "$out" | awk 'NR==1{print $1}')
  part=$(echo "$out" | awk '/Microsoft Basic Data/{print $1}')
  if ! fsck_msdos -n "$part" >/dev/null 2>&1; then echo "    [ERROR] fsck_msdos reports problems on $part"; ok=1; fi
  mkdir -p "$MNT"
  diskutil mount readOnly -mountPoint "$MNT" "$part" >/dev/null || { echo "[ERROR] read-only mount failed"; detach; return 1; }
  got_cfg=$(sha "$MNT/EFI/OC/config.plist")
  got_bak=$(sha "$MNT/EFI/OC/B00tGeni3/config.plist.bak")
  got_name=$(head -1 "$MNT/EFI/OC/B00tGeni3/active.txt" 2>/dev/null)
  echo "    config.plist     ${got_cfg:-<missing>}"
  [ "$got_cfg" = "$want_cfg" ] || { echo "    [ERROR] config.plist should be $want_cfg"; ok=1; }
  echo "    config.plist.bak ${got_bak:-<none>}"
  if [ "$want_bak" = NONE ]; then
    [ -z "$got_bak" ] || { echo "    [ERROR] a backup was written although every pick should have been refused"; ok=1; }
  else
    [ "$got_bak" = "$want_bak" ] || { echo "    [ERROR] backup should be $want_bak"; ok=1; }
  fi
  echo "    active.txt       ${got_name:-<none>}"
  if [ "$want_name" = NONE ]; then
    [ -z "$got_name" ] || { echo "    [ERROR] active.txt was written although every pick should have been refused"; ok=1; }
  else
    [ "$got_name" = "$want_name" ] || { echo "    [ERROR] active.txt should be $want_name"; ok=1; }
  fi
  for f in EFI/OC/config.tmp EFI/OC/config.old EFI/OC/B00tGeni3/config.bak.tmp EFI/OC/B00tGeni3/active.tmp; do
    [ -e "$MNT/$f" ] && { echo "    [ERROR] leftover $f"; ok=1; }
  done
  detach
  return $ok
}

# session <batch> <extra steps after the four refusals>
session() {
  local batch="$1" tail_steps="$2" steps="key:ret,wait:6,shot:p5$1-menu" n=2 v out
  for v in $(cd "$BROKEN/$batch" && ls *.plist | sort); do
    steps="$steps,key:$n,wait:3,shot:p5$batch-${v%.plist},key:spc,wait:2"
    n=$((n + 1))
  done
  out=$("$BENCH/bench.sh" --headless 35 --debug --extra-variants "$BROKEN/$batch" --steps "$steps$tail_steps" < /dev/null \
        | grep -E '^\[(ERROR|WARN)\]|extra variants|finished')
  echo "$out"
  if echo "$out" | grep -qE "screenshot not written|did not quit|monitor did not answer"; then
    echo "    [ERROR] harness failure in batch $batch (QEMU monitor socket silent) - run the gate again"
    return 1
  fi
  return 0
}

KG=$(sha "$VAR/known-good.plist")
T1=$(sha "$VAR/test1.plist")
[ -n "$KG" ] && [ -n "$T1" ] || { echo "[ERROR] variants missing in $VAR"; exit 1; }
for b in a b; do
  [ "$(ls "$BROKEN/$b"/*.plist 2>/dev/null | wc -l)" -eq 4 ] || { echo "[ERROR] need 4 broken variants in $BROKEN/$b - run tools/make-broken-variants.py"; exit 1; }
done

exec > >(tee "$REPORT") 2>&1
echo "[INFO] B00tGeni3 Phase 5 gate - $(date)"
echo "[INFO] known-good.plist $KG"
echo "[INFO] test1.plist      $T1"
echo "[INFO] tool sha256 $(sha "$BENCH/ESP/EFI/OC/Tools/B00tGeni3.efi")"
for f in "$BROKEN"/a/*.plist "$BROKEN"/b/*.plist; do echo "[INFO] broken variant $(sha "$f") $(basename "$f")"; done

FAILS=0
echo "[INFO] batch a: menu keys 2-5 = bad-acpi, bad-driver, bad-kext, bad-tool - all must be refused"
session a "" || FAILS=$((FAILS + 1))
if check "$KG" NONE NONE; then echo "    batch a OK: nothing was changed on disk"; else echo "    [ERROR] batch a FAILED"; FAILS=$((FAILS + 1)); fi

echo "[INFO] batch b: menu keys 2-5 = bad-nopicker, bad-notool, bad-parse, bad-vault - all must be refused;"
echo "[INFO]          then key 6 = test1.plist, which must pass pre-flight and swap (positive control)"
session b ",key:6,wait:3,shot:p5b-test1-preflight,key:y,wait:1,shot:p5b-test1-switching,wait:40,shot:p5b-test1-after" || FAILS=$((FAILS + 1))
if check "$T1" "$KG" test1.plist; then echo "    batch b OK: only test1.plist was activated"; else echo "    [ERROR] batch b FAILED"; FAILS=$((FAILS + 1)); fi

echo "[INFO] failures: $FAILS"
echo "[INFO] screenshots: $BENCH/logs/*-p5a-*.png and *-p5b-*.png"
DONE=1
[ "$FAILS" -eq 0 ]
