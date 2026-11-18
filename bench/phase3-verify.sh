#!/bin/bash
# B00tGeni3 Phase 3 host-side check.
# Runs on: a macOS 12.7.6 build host, normal user. QEMU must have exited first.
# Attaches the bench ESP image READ-ONLY, runs fsck_msdos -n, and checks that
# EFI/OC/B00tGeni3/test.bin has the expected sha256 and that test.tmp is gone.
#
# Usage: phase3-verify.sh [image]   (default: bench/run/esp.cdr)

BENCH="$(cd "$(dirname "$0")" && pwd)"
IMG="${1:-$BENCH/run/esp.cdr}"
MNT="$BENCH/run/verify-mnt"
DEV=""

cleanup() {
  rc=$?
  if [ -n "$DEV" ]; then
    diskutil unmount "$MNT" >/dev/null 2>&1
    hdiutil detach "$DEV" >/dev/null 2>&1 || echo "[WARN] could not detach $DEV - run: hdiutil detach $DEV"
  fi
  rmdir "$MNT" 2>/dev/null
  if [ "$rc" -eq 0 ]; then echo "[INFO] PHASE 3 HOST CHECK: PASS"; else echo "[ERROR] PHASE 3 HOST CHECK: FAIL (exit $rc)"; fi
  exit "$rc"
}
trap cleanup EXIT

[ -s "$IMG" ] || { echo "[ERROR] image missing: $IMG"; exit 1; }
HOLDERS=$(lsof -t "$IMG" 2>/dev/null)
[ -n "$HOLDERS" ] && { echo "[ERROR] $IMG is open by pid(s) $HOLDERS - quit QEMU first"; exit 1; }
echo "[INFO] image: $IMG"
echo "[INFO] image sha256: $(shasum -a 256 "$IMG" | awk '{print $1}')"

EXPECTED=$(python3 -I -c '
import hashlib
h = b"B00tGeni3 Phase 3 write test v1\n"
d = bytearray(h) + bytes(((i * 31 + 7) & 0xFF) for i in range(len(h), 4096))
assert len(d) == 4096
print(hashlib.sha256(bytes(d)).hexdigest())')
[ -n "$EXPECTED" ] || { echo "[ERROR] could not compute expected hash"; exit 1; }
echo "[INFO] expected test.bin sha256: $EXPECTED"

OUT=$(hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" 2>&1) || { echo "[ERROR] hdiutil attach failed: $OUT"; exit 1; }
DEV=$(echo "$OUT" | awk 'NR==1{print $1}')
PART=$(echo "$OUT" | awk '/Microsoft Basic Data/{print $1}')
[ -n "$PART" ] || { echo "[ERROR] no FAT partition found:"; echo "$OUT"; exit 1; }
echo "[INFO] attached read-only: $DEV, FAT partition $PART"

echo "[INFO] fsck_msdos -n $PART"
fsck_msdos -n "$PART" 2>&1 | sed 's/^/    /'
FSCK=${PIPESTATUS[0]}
[ "$FSCK" -eq 0 ] && echo "[INFO] fsck_msdos: clean" || echo "[WARN] fsck_msdos exit $FSCK - see output above"

mkdir -p "$MNT" || { echo "[ERROR] cannot create $MNT"; exit 1; }
diskutil mount readOnly -mountPoint "$MNT" "$PART" >/dev/null || { echo "[ERROR] read-only mount failed"; exit 1; }
echo "[INFO] mounted read-only at $MNT"
echo "[INFO] EFI/OC/B00tGeni3:"
ls -la "$MNT/EFI/OC/B00tGeni3" 2>&1 | sed 's/^/    /'

FAIL=0
[ -e "$MNT/EFI/OC/B00tGeni3/test.tmp" ] && { echo "[ERROR] test.tmp still present (rename did not complete)"; FAIL=1; }
if [ -f "$MNT/EFI/OC/B00tGeni3/test.bin" ]; then
  GOT=$(shasum -a 256 "$MNT/EFI/OC/B00tGeni3/test.bin" | awk '{print $1}')
  SIZE=$(stat -f %z "$MNT/EFI/OC/B00tGeni3/test.bin")
  echo "[INFO] test.bin size $SIZE sha256 $GOT"
  [ "$GOT" = "$EXPECTED" ] || { echo "[ERROR] test.bin hash mismatch"; FAIL=1; }
else
  echo "[ERROR] test.bin not found"; FAIL=1
fi
[ "$FSCK" -eq 0 ] || FAIL=1
exit $FAIL
