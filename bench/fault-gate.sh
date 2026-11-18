#!/bin/bash
# B00tGeni3 fault tests (the Phase 4 items oTR-DaRkNeSs deferred): 10-swap run and power cut during a swap.
# Runs on: a macOS 12.7.6 build host, normal user. Works on the bench image run/esp.cdr only.
# Keys are for the 0.8d/0.8e screen: the tool starts with the cursor on the first variant
# (known-good.plist); Enter runs the pre-flight; Down + Enter answers YES on the confirm page.
#
# Usage: fault-gate.sh swaps [N]          N swaps known-good <-> test1, host check after each (default 10)
#        fault-gate.sh powercut "MS ..."  one swap per value; QEMU is killed MS milliseconds after the
#                                         YES key. Then the image is checked and booted again.

BENCH="$(cd "$(dirname "$0")" && pwd)"
MODE="${1:-swaps}"
IMG="$BENCH/run/esp.cdr"
MNT="$BENCH/run/gate-mnt"
VAR="$BENCH/ESP/EFI/OC/B00tGeni3"
STAMP=$(date +%Y%m%d-%H%M%S)
REPORT="$BENCH/logs/$STAMP-fault-$MODE.txt"
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
  if [ "$rc" -eq 0 ]; then echo "[INFO] FAULT TEST $MODE: PASS"; else echo "[ERROR] FAULT TEST $MODE: FAIL (exit $rc)"; fi
  exit "$rc"
}
trap finish EXIT

sha() { shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'; }

# Attach the image read-only and fill GOT_CFG GOT_BAK GOT_NAME FSCK LEFT. Returns 1 if it cannot attach.
inspect() {
  local out part f
  out=$(hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" 2>&1) || { echo "[ERROR] attach failed: $out"; return 1; }
  DEV=$(echo "$out" | awk 'NR==1{print $1}')
  part=$(echo "$out" | awk '/Microsoft Basic Data/{print $1}')
  FSCK=clean
  FSCK_OUT=$(fsck_msdos -n "$part" 2>&1) || FSCK=PROBLEMS
  FSCK_OUT=$(echo "$FSCK_OUT" | grep -vE '^\*\*|^[0-9]+ files|\? no$')
  mkdir -p "$MNT"
  diskutil mount readOnly -mountPoint "$MNT" "$part" >/dev/null || { echo "[ERROR] read-only mount failed"; detach; return 1; }
  GOT_CFG=$(sha "$MNT/EFI/OC/config.plist")
  GOT_BAK=$(sha "$MNT/EFI/OC/B00tGeni3/config.plist.bak")
  GOT_NAME=$(head -1 "$MNT/EFI/OC/B00tGeni3/active.txt" 2>/dev/null)
  LEFT=""
  for f in EFI/OC/config.tmp EFI/OC/config.old EFI/OC/B00tGeni3/config.bak.tmp EFI/OC/B00tGeni3/active.tmp; do
    [ -e "$MNT/$f" ] && LEFT="$LEFT $f"
  done
  detach
  return 0
}

name_of() { case "$1" in "$KG") echo known-good ;; "$T1") echo test1 ;; "") echo "<missing>" ;; *) echo "UNKNOWN($1)" ;; esac; }

KG=$(sha "$VAR/known-good.plist")
T1=$(sha "$VAR/test1.plist")
[ -n "$KG" ] && [ -n "$T1" ] || { echo "[ERROR] variants missing in $VAR"; exit 1; }

exec > >(tee "$REPORT") 2>&1
echo "[INFO] B00tGeni3 fault test '$MODE' - $(date)"
echo "[INFO] known-good.plist $KG"
echo "[INFO] test1.plist      $T1"
echo "[INFO] tool sha256 $(sha "$BENCH/ESP/EFI/OC/Tools/B00tGeni3.efi")"

echo "[INFO] round 0: fresh image, boot to picker"
"$BENCH/bench.sh" --headless 25 < /dev/null | grep -E '^\[(ERROR|WARN)\]|finished' || exit 1
inspect || exit 1
[ "$GOT_CFG" = "$KG" ] || { echo "[ERROR] fresh image config is not known-good"; exit 1; }

# Keys that move from the tool's start position to the wanted variant and press Enter on it.
pick_keys() { if [ "$1" = test1 ]; then echo "key:down,wait:1,key:ret"; else echo "key:ret"; fi; }

FAILS=0
case "$MODE" in
swaps)
  SWAPS="${2:-10}"
  PREV="$KG"
  RETRIES=0
  for i in $(seq 1 "$SWAPS"); do
    if [ $((i % 2)) -eq 1 ]; then TO=test1; WANT="$T1"; else TO=known-good; WANT="$KG"; fi
    echo "[INFO] swap $i/$SWAPS -> $TO.plist"
    for try in 1 2; do
      out=$("$BENCH/bench.sh" --headless 25 --keep-image \
        --steps "key:ret,wait:8,$(pick_keys $TO),wait:4,key:down,wait:1,key:ret,wait:25,key:ret,wait:8,shot:fswap$i-after" \
        < /dev/null | grep -E '^\[(ERROR|WARN)\]|finished')
      echo "$out"
      echo "$out" | grep -q "screenshot not written" || break
      echo "    [WARN] harness failure (monitor socket silent) - retrying this swap once"; RETRIES=$((RETRIES + 1))
    done
    inspect || exit 1
    echo "    fsck $FSCK | config $(name_of "$GOT_CFG") | bak $(name_of "$GOT_BAK") | active.txt ${GOT_NAME:-<missing>} | leftovers:${LEFT:- none}"
    if [ "$FSCK" = clean ] && [ "$GOT_CFG" = "$WANT" ] && [ "$GOT_BAK" = "$PREV" ] && [ "$GOT_NAME" = "$TO.plist" ] && [ -z "$LEFT" ]; then
      echo "    swap $i OK"
    else
      echo "    [ERROR] swap $i FAILED"; FAILS=$((FAILS + 1))
    fi
    PREV="$WANT"
  done
  echo "[INFO] $((SWAPS - FAILS))/$SWAPS swaps passed, harness retries: $RETRIES"
  ;;
powercut)
  # Every cut starts from the same pristine image, so damage from one cut cannot leak into the next.
  cp "$IMG" "$IMG.pristine" || exit 1
  CUR="$KG"
  n=0
  for ms in ${2:-0 50 100 200 400}; do
    n=$((n + 1))
    cp "$IMG.pristine" "$IMG" || exit 1
    CUR="$KG"
    if [ "$CUR" = "$KG" ]; then TO=test1; WANT="$T1"; else TO=known-good; WANT="$KG"; fi
    echo "[INFO] cut $n: swap -> $TO.plist, power cut ${ms}ms after YES"
    "$BENCH/bench.sh" --headless 25 --keep-image \
      --steps "key:ret,wait:8,$(pick_keys $TO),wait:4,key:down,wait:1,key:ret,msleep:$ms,kill" \
      < /dev/null | grep -E '^\[(ERROR|WARN)\]|finished'
    inspect || exit 1
    echo "    after cut : fsck $FSCK | config $(name_of "$GOT_CFG") | bak $(name_of "$GOT_BAK") | active.txt ${GOT_NAME:-<missing>} | leftovers:${LEFT:- none}"
    # The one hard requirement: config.plist is a whole known file, old or new.
    if [ "$GOT_CFG" != "$KG" ] && [ "$GOT_CFG" != "$T1" ]; then
      echo "    [ERROR] config.plist is neither variant after the cut"; FAILS=$((FAILS + 1))
    fi
    [ "$FSCK" = clean ] || { echo "    [WARN] fsck reports problems after the cut:"; echo "$FSCK_OUT" | sed 's/^/        /'; }
    cp "$IMG" "$BENCH/run/cut-$STAMP-${ms}ms.cdr"
    CUT_CFG="$GOT_CFG"
    # Boot again: picker must come up, tool must start and show its screen.
    "$BENCH/bench.sh" --headless 25 --keep-image --steps "key:ret,wait:8,shot:cut$n-${ms}ms-reboot" \
      < /dev/null | grep -E '^\[(ERROR|WARN)\]|finished'
    inspect || exit 1
    echo "    after boot: fsck $FSCK | config $(name_of "$GOT_CFG") | active.txt ${GOT_NAME:-<missing>} | leftovers:${LEFT:- none}"
    [ "$GOT_CFG" = "$CUT_CFG" ] || echo "    [WARN] config changed during the recovery boot"
    CUR="$GOT_CFG"
  done
  echo "[INFO] cuts with a broken config.plist: $FAILS"
  ;;
recover)
  # One swap on each image left by an earlier power-cut run (fault-gate.sh recover STAMP):
  # can the user get out of the state the cut left behind? The image keeps the tool it was cut with.
  for img in "$BENCH"/run/cut-"$2"-*ms.cdr; do
    [ -f "$img" ] || { echo "[ERROR] no images run/cut-$2-*ms.cdr"; exit 1; }
    tag=$(basename "$img" .cdr)
    cp "$img" "$IMG" || exit 1
    inspect || exit 1
    BEFORE="$GOT_CFG"
    if [ "$BEFORE" = "$KG" ]; then TO=test1; WANT="$T1"; else TO=known-good; WANT="$KG"; fi
    echo "[INFO] $tag: before fsck $FSCK | config $(name_of "$BEFORE") | leftovers:${LEFT:- none} -> swap to $TO.plist"
    "$BENCH/bench.sh" --headless 25 --keep-image \
      --steps "key:ret,wait:8,shot:$tag-rec-main,$(pick_keys $TO),wait:4,shot:$tag-rec-pre,key:down,wait:1,key:ret,wait:25,key:ret,wait:8,shot:$tag-rec-after" \
      < /dev/null | grep -E '^\[(ERROR|WARN)\]|finished'
    inspect || exit 1
    echo "    after: fsck $FSCK | config $(name_of "$GOT_CFG") | bak $(name_of "$GOT_BAK") | active.txt ${GOT_NAME:-<missing>} | leftovers:${LEFT:- none}"
    if [ "$GOT_CFG" = "$WANT" ]; then echo "    recovery swap OK"; else echo "    [ERROR] recovery swap did not happen"; FAILS=$((FAILS + 1)); fi
  done
  echo "[INFO] cut images where the next swap failed: $FAILS"
  ;;
*)
  echo "[ERROR] unknown mode $MODE"; exit 1 ;;
esac
[ "$FAILS" -eq 0 ]
