#!/bin/bash
# B00tGeni3 QEMU test bench (Phase 1).
# Runs on: a macOS 12.7.6 build host, normal user. Needs MacPorts qemu (/opt/local).
# Boots the master EFI folder bench/ESP/EFI with OVMF. ocvalidate runs before every boot.
#
# Usage:
#   bench.sh                 window mode (Cocoa display), waits for Enter at the end
#   bench.sh --headless [N]  no display; after N seconds (default 25) takes a screenshot and quits
#   --keep-vars              reuse run/OVMF_VARS.fd instead of a fresh copy of the template
#   --debug                  image gets the Phase 0 DEBUG X64 OpenCore.efi (master folder stays RELEASE)
#   --keep-image             boot the existing run/esp.cdr again instead of rebuilding it from ESP/
#   --extra-variants DIR     copy DIR/*.plist into the image's EFI/OC/B00tGeni3 (master folder untouched)
#   --resolution WxH         set UEFI > Output > Resolution in the image's config.plist only (e.g. 1024x768)
#   --steps "a,b,..."        headless only, after the picker screenshot: key:<qemu key>, wait:<sec>, shot:<name>
#                            e.g. --steps "key:ret,wait:5,shot:tool,key:spc,wait:5,shot:back"
#                            also msleep:<ms> and kill (SIGKILL = power cut)
#   --esp DIR                master folder to boot instead of bench/ESP (e.g. bench/ESP-vault)
#   --oc-debug FILE          OpenCore.efi used by --debug instead of the Phase 0 DEBUG build (a key-patched copy)
#   --overlay DIR            copy DIR's tree over the staged image (e.g. DIR/EFI/OC/config.plist), master untouched
#   --remove PATH            delete PATH (relative to the image root) from the staged image; repeatable
#
# Output per run in bench/logs/: <stamp>-serial.log, <stamp>-qemu.log, <stamp>-picker.png (headless)

BENCH="$(cd "$(dirname "$0")" && pwd)"
ESP="$BENCH/ESP"
OCVALIDATE="$BENCH/../phase0/OpenCorePkg-1.0.8/Utilities/ocvalidate/ocvalidate"
QEMU=/opt/local/bin/qemu-system-x86_64
OVMF_CODE=/opt/local/share/qemu/edk2-x86_64-code.fd
OVMF_VARS_TEMPLATE=/opt/local/share/qemu/edk2-i386-vars.fd
RUN="$BENCH/run"
LOGS="$BENCH/logs"
STAMP=$(date +%Y%m%d-%H%M%S)
SERIAL_LOG="$LOGS/$STAMP-serial.log"
QEMU_LOG="$LOGS/$STAMP-qemu.log"
SHOT="$LOGS/$STAMP-picker.png"
MON="$RUN/mon.sock"

OC_DEBUG_EFI="$BENCH/../phase0/OpenCorePkg-1.0.8/UDK/Build/OpenCorePkg/DEBUG_XCODE5/X64/OpenCore.efi"

MODE=window
WAIT=25
KEEP_VARS=0
OC_BUILD=RELEASE
STEPS=""
KEEP_IMAGE=0
EXTRA_VARIANTS=""
RESOLUTION=""
OVERLAY=""
REMOVE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --headless) MODE=headless; if [[ "${2:-}" =~ ^[0-9]+$ ]]; then WAIT=$2; shift; fi ;;
    --keep-vars) KEEP_VARS=1 ;;
    --debug) OC_BUILD=DEBUG ;;
    --steps) STEPS="${2:-}"; shift ;;
    --keep-image) KEEP_IMAGE=1 ;;
    --extra-variants) EXTRA_VARIANTS="${2:-}"; shift ;;
    --resolution) RESOLUTION="${2:-}"; shift ;;
    --esp) ESP="${2:-}"; shift ;;
    --oc-debug) OC_DEBUG_EFI="${2:-}"; shift ;;
    --overlay) OVERLAY="${2:-}"; shift ;;
    --remove) REMOVE="$REMOVE ${2:-}"; shift ;;
    *) echo "[ERROR] unknown argument: $1"; exit 2 ;;
  esac
  shift
done
CONFIG="$ESP/EFI/OC/config.plist"

QPID=""
finish() {
  rc=$?
  [ -n "$QPID" ] && kill -0 "$QPID" 2>/dev/null && { echo "[WARN] QEMU still running - killing pid $QPID"; kill "$QPID" 2>/dev/null; }
  rm -f "$MON"
  echo
  if [ "$rc" -eq 0 ]; then echo "[INFO] bench finished OK"; else echo "[ERROR] bench finished with exit code $rc"; fi
  echo "[INFO] serial log: $SERIAL_LOG"
  echo "[INFO] qemu log:   $QEMU_LOG"
  [ -f "$SHOT" ] && echo "[INFO] screenshot: $SHOT"
  if [ -t 0 ]; then read -r -p "Press Enter to close..." _; fi
  exit "$rc"
}
trap finish EXIT

echo "[INFO] B00tGeni3 bench - mode=$MODE - $(date)"
for f in "$QEMU" "$OVMF_CODE" "$OVMF_VARS_TEMPLATE" "$OCVALIDATE" "$CONFIG" "$ESP/EFI/BOOT/BOOTx64.efi" "$ESP/EFI/OC/OpenCore.efi"; do
  [ -e "$f" ] || { echo "[ERROR] missing: $f"; exit 1; }
done
mkdir -p "$RUN" "$LOGS" || { echo "[ERROR] cannot create $RUN or $LOGS"; exit 1; }
BUSY=$(lsof -t "$RUN/esp.cdr" "$RUN/OVMF_VARS.fd" 2>/dev/null | sort -u | tr '\n' ' ')
[ -n "$BUSY" ] && { echo "[ERROR] another bench QEMU is running (pid $BUSY) - close it first"; exit 1; }

echo "[INFO] ocvalidate $CONFIG"
"$OCVALIDATE" "$CONFIG"
ov=$?
[ "$ov" -eq 0 ] || { echo "[ERROR] ocvalidate failed (exit $ov) - not booting"; exit 1; }

if [ "$KEEP_VARS" -eq 1 ] && [ -f "$RUN/OVMF_VARS.fd" ]; then
  echo "[INFO] reusing $RUN/OVMF_VARS.fd"
else
  cp "$OVMF_VARS_TEMPLATE" "$RUN/OVMF_VARS.fd" || { echo "[ERROR] cannot copy OVMF vars template"; exit 1; }
  echo "[INFO] fresh NVRAM: $RUN/OVMF_VARS.fd"
fi

# ESP disk: a fresh 64 MB GPT + FAT32 raw image built from the master folder every run.
# QEMU writes only ever reach this copy (run/esp.cdr), never the master folder.
ESP_IMG="$RUN/esp.cdr"
STAGE="$RUN/esp-stage"
if [ "$KEEP_IMAGE" -eq 1 ]; then
  [ -s "$ESP_IMG" ] || { echo "[ERROR] --keep-image but no image at $ESP_IMG"; exit 1; }
  echo "[INFO] reusing existing ESP image (--keep-image): $ESP_IMG"
else
  rm -rf "$ESP_IMG" "$STAGE"
  ditto "$ESP" "$STAGE" || { echo "[ERROR] cannot stage $ESP"; exit 1; }
  if [ "$OC_BUILD" = DEBUG ]; then
    [ -f "$OC_DEBUG_EFI" ] || { echo "[ERROR] missing DEBUG OpenCore.efi: $OC_DEBUG_EFI"; exit 1; }
    cp "$OC_DEBUG_EFI" "$STAGE/EFI/OC/OpenCore.efi" || { echo "[ERROR] cannot swap in DEBUG OpenCore.efi"; exit 1; }
  fi
  if [ -n "$EXTRA_VARIANTS" ]; then
    ls "$EXTRA_VARIANTS"/*.plist >/dev/null 2>&1 || { echo "[ERROR] no .plist files in $EXTRA_VARIANTS"; exit 1; }
    cp "$EXTRA_VARIANTS"/*.plist "$STAGE/EFI/OC/B00tGeni3/" || { echo "[ERROR] cannot copy extra variants"; exit 1; }
    echo "[INFO] extra variants in image: $(cd "$EXTRA_VARIANTS" && ls *.plist | tr '\n' ' ')"
  fi
  if [ -n "$RESOLUTION" ]; then
    /usr/libexec/PlistBuddy -c "Set :UEFI:Output:Resolution $RESOLUTION" "$STAGE/EFI/OC/config.plist" \
      || { echo "[ERROR] cannot set Resolution $RESOLUTION in the staged config"; exit 1; }
    "$OCVALIDATE" "$STAGE/EFI/OC/config.plist" >/dev/null || { echo "[ERROR] ocvalidate rejects the staged config with Resolution $RESOLUTION"; exit 1; }
    echo "[INFO] image config: UEFI > Output > Resolution = $RESOLUTION (master folder unchanged)"
  fi
  if [ -n "$OVERLAY" ]; then
    [ -d "$OVERLAY" ] || { echo "[ERROR] --overlay folder missing: $OVERLAY"; exit 1; }
    ditto "$OVERLAY" "$STAGE" || { echo "[ERROR] cannot copy overlay $OVERLAY"; exit 1; }
    echo "[INFO] overlay in image: $(cd "$OVERLAY" && find . -type f | sed 's|^\./||' | tr '\n' ' ')"
  fi
  for r in $REMOVE; do
    [ -e "$STAGE/$r" ] || { echo "[ERROR] --remove: $r not in the staged image"; exit 1; }
    rm -f "$STAGE/$r" && echo "[INFO] removed from image: $r"
  done
  echo "[INFO] ESP master: $ESP"
  echo "[INFO] OpenCore.efi in image: $OC_BUILD, sha256 $(shasum -a 256 "$STAGE/EFI/OC/OpenCore.efi" | awk '{print $1}')"
  echo "[INFO] building ESP image from $STAGE"
  hdiutil create -quiet -size 64m -fs "MS-DOS FAT32" -volname OCESP -layout GPTSPUD -format UDTO -srcfolder "$STAGE" -o "$RUN/esp" \
    || { echo "[ERROR] hdiutil could not build $ESP_IMG"; exit 1; }
  [ -s "$ESP_IMG" ] || { echo "[ERROR] ESP image missing: $ESP_IMG"; exit 1; }
fi
echo "[INFO] ESP image sha256: $(shasum -a 256 "$ESP_IMG" | awk '{print $1}')"
ESP_DRIVE=(-drive "if=none,id=esp,format=raw,file=$ESP_IMG" -device ahci,id=ahci -device ide-hd,drive=esp,bus=ahci.0,bootindex=0)

if [ "$MODE" = headless ]; then DISPLAY_ARGS=(-display none); else DISPLAY_ARGS=(-display cocoa); fi

ARGS=(-machine q35 -accel hvf -accel tcg -m 2048 -smp 2
  -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
  -drive "if=pflash,format=raw,file=$RUN/OVMF_VARS.fd"
  "${ESP_DRIVE[@]}"
  -serial "file:$SERIAL_LOG"
  -monitor "unix:$MON,server,nowait"
  -net none
  "${DISPLAY_ARGS[@]}")

rm -f "$MON"
echo "[INFO] starting QEMU: $QEMU ${ARGS[*]}"
"$QEMU" "${ARGS[@]}" >"$QEMU_LOG" 2>&1 &
QPID=$!
sleep 2
kill -0 "$QPID" 2>/dev/null || { echo "[ERROR] QEMU exited at start:"; cat "$QEMU_LOG"; QPID=""; exit 1; }
grep -qi hvf "$QEMU_LOG" && echo "[WARN] QEMU reported an hvf problem - see $QEMU_LOG (tcg fallback may be in use)"

# Send one monitor command and check QEMU answered with its prompt. Up to 3 tries.
# If the monitor stays silent, a stack sample of QEMU is saved once for diagnosis.
MON_SAMPLED=0
mon() {
  local reply try
  for try in 1 2 3; do
    reply=$(printf '%s\n' "$1" | nc -U -w 2 "$MON" 2>/dev/null)
    case "$reply" in *"(qemu)"*) [ "$try" -gt 1 ] && echo "[WARN] monitor answered on try $try: $1"; return 0 ;; esac
    sleep 1
  done
  echo "[WARN] monitor did not answer: $1"
  if [ "$MON_SAMPLED" -eq 0 ] && [ -n "$QPID" ]; then
    MON_SAMPLED=1
    sample "$QPID" 2 -file "$LOGS/$STAMP-qemu-sample.txt" >/dev/null 2>&1 && echo "[WARN] QEMU stack sample: $LOGS/$STAMP-qemu-sample.txt"
  fi
  return 1
}

if [ "$MODE" = headless ]; then
  echo "[INFO] waiting ${WAIT}s for the picker..."
  sleep "$WAIT"
  kill -0 "$QPID" 2>/dev/null || { echo "[ERROR] QEMU exited early:"; cat "$QEMU_LOG"; QPID=""; exit 1; }
  mon "screendump $SHOT -f png"
  sleep 2
  [ -s "$SHOT" ] || echo "[WARN] screenshot not written: $SHOT"
  IFS=',' read -r -a STEP_LIST <<< "$STEPS"
  for step in "${STEP_LIST[@]}"; do
    [ -n "$step" ] || continue
    case "$step" in
      key:*)  echo "[INFO] step: sendkey ${step#key:}"; mon "sendkey ${step#key:}" ;;
      wait:*) echo "[INFO] step: wait ${step#wait:}s"; sleep "${step#wait:}" ;;
      shot:*) f="$LOGS/$STAMP-${step#shot:}.png"; echo "[INFO] step: screenshot $f"; mon "screendump $f -f png"; sleep 2
              [ -s "$f" ] || echo "[WARN] screenshot not written: $f" ;;
      msleep:*) echo "[INFO] step: wait ${step#msleep:}ms"; perl -e "select(undef,undef,undef,${step#msleep:}/1000)" ;;
      kill)   # power cut: SIGKILL, no monitor quit, nothing flushed by QEMU itself
              echo "[INFO] step: POWER CUT (kill -9 $QPID)"; kill -9 "$QPID"; wait "$QPID" 2>/dev/null; QPID=""
              echo "[INFO] bench finished OK (power cut)"; exit 0 ;;
      *)      echo "[WARN] unknown step ignored: $step" ;;
    esac
    kill -0 "$QPID" 2>/dev/null || { echo "[ERROR] QEMU exited during steps"; QPID=""; exit 1; }
  done
  mon "quit"
  for _ in 1 2 3 4 5; do kill -0 "$QPID" 2>/dev/null || break; sleep 1; done
  kill -0 "$QPID" 2>/dev/null && { echo "[WARN] QEMU did not quit - killing"; kill "$QPID"; }
  wait "$QPID" 2>/dev/null
  QPID=""
else
  echo "[INFO] QEMU window open - close it to finish"
  wait "$QPID"
  qrc=$?
  QPID=""
  [ "$qrc" -eq 0 ] || { echo "[ERROR] QEMU exited with code $qrc"; cat "$QEMU_LOG"; exit 1; }
fi

echo "[INFO] OpenCore lines in serial log:"
grep -a 'OC' "$SERIAL_LOG" | head -40 || true
exit 0
