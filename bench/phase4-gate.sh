#!/bin/bash
# B00tGeni3 Phase 4 gate: swap known-good -> test1 -> known-good ... 10 times (5 round trips).
# Runs on: a macOS 12.7.6 build host, normal user.
# One fresh ESP image is built from bench/ESP, then every swap is its own QEMU session on that
# same image (bench.sh --keep-image). After each session the image is attached READ-ONLY and
# checked: fsck clean, config.plist == chosen variant, config.plist.bak == previous config,
# active.txt == chosen name, no config.tmp / config.old left behind.
#
# Usage: phase4-gate.sh [swaps]   (default 10)

BENCH="$(cd "$(dirname "$0")" && pwd)"
SWAPS="${1:-10}"
IMG="$BENCH/run/esp.cdr"
MNT="$BENCH/run/gate-mnt"
VAR="$BENCH/ESP/EFI/OC/B00tGeni3"
STAMP=$(date +%Y%m%d-%H%M%S)
REPORT="$BENCH/logs/$STAMP-phase4-gate.txt"
DEV=""

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
  if [ "$rc" -eq 0 ]; then echo "[INFO] PHASE 4 GATE: PASS"; else echo "[ERROR] PHASE 4 GATE: FAIL (exit $rc)"; fi
  exit "$rc"
}
trap finish EXIT

sha() { shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'; }

# check <expected config sha> <expected backup sha or -> <expected active name or ->
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
  if [ "$want_bak" != "-" ]; then
    echo "    config.plist.bak ${got_bak:-<missing>}"
    [ "$got_bak" = "$want_bak" ] || { echo "    [ERROR] backup should be $want_bak"; ok=1; }
  fi
  if [ "$want_name" != "-" ]; then
    echo "    active.txt       ${got_name:-<missing>}"
    [ "$got_name" = "$want_name" ] || { echo "    [ERROR] active.txt should be $want_name"; ok=1; }
  fi
  for f in EFI/OC/config.tmp EFI/OC/config.old EFI/OC/B00tGeni3/config.bak.tmp EFI/OC/B00tGeni3/active.tmp; do
    [ -e "$MNT/$f" ] && { echo "    [ERROR] leftover $f"; ok=1; }
  done
  detach
  return $ok
}

KG=$(sha "$VAR/known-good.plist")
T1=$(sha "$VAR/test1.plist")
[ -n "$KG" ] && [ -n "$T1" ] || { echo "[ERROR] variants missing in $VAR"; exit 1; }

exec > >(tee "$REPORT") 2>&1
echo "[INFO] B00tGeni3 Phase 4 gate - $SWAPS swaps - $(date)"
echo "[INFO] known-good.plist $KG"
echo "[INFO] test1.plist      $T1"
echo "[INFO] tool sha256 $(sha "$BENCH/ESP/EFI/OC/Tools/B00tGeni3.efi")"

echo "[INFO] round 0: fresh image, boot to picker"
"$BENCH/bench.sh" --headless 35 --debug < /dev/null | grep -E '^\[(ERROR|WARN)\]|screenshot|finished' || exit 1
check "$KG" - - || exit 1

# One QEMU session for swap $i. Returns 2 if the harness itself failed (QEMU monitor socket
# did not answer, so no keys were sent and the tool never ran).
run_swap() {
  local out
  out=$("$BENCH/bench.sh" --headless 35 --debug --keep-image \
    --steps "key:ret,wait:6,shot:swap$i-menu,key:$KEY,wait:2,key:y,wait:1,shot:swap$i-switching,wait:40,shot:swap$i-after" \
    < /dev/null | grep -E '^\[(ERROR|WARN)\]|finished')
  echo "$out"
  echo "$out" | grep -q "screenshot not written" && return 2
  return 0
}

PREV="$KG"
FAILS=0
RETRIES=0
for i in $(seq 1 "$SWAPS"); do
  if [ $((i % 2)) -eq 1 ]; then KEY=2; NAME=test1.plist; WANT="$T1"; else KEY=1; NAME=known-good.plist; WANT="$KG"; fi
  echo "[INFO] swap $i/$SWAPS -> $NAME (menu key $KEY)"
  run_swap
  if [ $? -eq 2 ]; then
    # A retry after a swap that did happen would leave backup == new config, and check fails.
    echo "    [WARN] harness failure on swap $i (monitor socket silent) - retrying this swap once"
    RETRIES=$((RETRIES + 1))
    run_swap
  fi
  if check "$WANT" "$PREV" "$NAME"; then
    echo "    swap $i OK"
  else
    echo "    [ERROR] swap $i FAILED"
    FAILS=$((FAILS + 1))
  fi
  PREV="$WANT"
done

echo "[INFO] $((SWAPS - FAILS))/$SWAPS swaps passed, harness retries: $RETRIES"
[ "$FAILS" -eq 0 ]
