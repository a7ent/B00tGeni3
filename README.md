# B00tGeni3 ☠

**OpenCore's missing boot configurer utility.**
The Swiss Army boot configurer that lives right inside your OpenCore picker.

~~ B00tGeni3 ☠ 2026 ~~ By: oTR-DaRkNeSs x CL4UD3-C0D3 `<cc/>`

> Status: **0.8g, tested in QEMU (OVMF + OpenCore 1.0.8) and on multiple real systems, vault included.**
> Use it on a machine you can recover with a USB installer or a second EFI.

![main screen draft](docs/drafts/draft-A2.png)

## Overview

Met with a kernel panic or a stuck Apple logo after a settings change, an update or an upgrade?
You know the drill. Dig out a USB installer, boot from it, mount the EFI, fix `config.plist`,
reboot and pray. Even trying a new kext, a different SMBIOS or verbose mode means booting into
macOS, editing the plist and hoping it comes back up.

B00tGeni3 cuts all of that out. It's a small UEFI tool that sits in `EFI\OC\Tools` and shows up as
an entry in the OpenCore picker. Pick it **before** the OS loads and you can:
- swap your whole config for another one
- change boot-args
- reset NVRAM or flip SIP
- dump what you need for debugging

Then head straight back to the picker. No OS, no mounting, no plist editors.

Keep a stack of configs on the ESP (your daily driver, a debug build, a test build and a
known-good fallback) and switch between them in seconds. B00tGeni3 checks every one before it lets
it near your boot, so it won't hand OpenCore a config that leaves you stuck at a black screen.

Built for Hackintosh tinkerers, multi-boot setups and anyone who's ever bricked their own boot at 2 a.m.

## Features

### ☠ CONFIGS: the config switcher
- Lists every config variant you keep in `EFI\OC\B00tGeni3\` (up to 9), with `known-good.plist` on top.
- Marks the one that's live right now as **ACTIVE**, and shows a quick overview of its contents.
- Pick a variant, confirm once, and it's swapped in with a cold reset.
- **Safe swap:**
  1. It backs up your current config.
  2. It writes the new one to a temp file and checks it.
  3. It renames it into place and checks it again.
  4. There is always a whole config on disk, never half of one.
- **RESTORE-FAILSAFE:** one key takes you back to `known-good.plist`.

### ☠ PRE-FLIGHT: the "don't brick me" check
Before any swap, the variant has to pass all of these:
- OpenCore's own parser accepts it.
- The picker still shows (`ShowPicker` on).
- B00tGeni3 is still in the Tools list, so you can always get back in.
- Every enabled driver, kext, ACPI table and tool actually exists on disk.
- The vault check passes, if you use a vault.

Fail any one and it's refused, with every reason listed.

### ☠ BOOT-ARGS: the boot-arg workshop
- One-key toggles: **VERBOSE** (`-v`), **SAFE BOOT** (`-x`), **SINGLE USER** (`-s`) and
  **DEBUG** (`debug=0x100`).
- **ADDITIONAL-ARGS-LIST:** a menu of common documented boot-args, each with a short description.
- A free edit line for anything else.
- Three ways to use it:
  - **COMMIT** saves the line to `config.plist`.
  - **APPLY & REBOOT** saves it and restarts.
  - **B00T ONCE** sets the args in NVRAM for the next boot only and leaves your config alone.

### ☠ VAULT SUPPORT: for locked-down setups
- Works with OpenCore's Vault, the signed-EFI setup.
- Each variant can carry its own signed vault set, which you sign on your Mac with the included
  `tools/vault-sign.py` (one key for every variant).
- B00tGeni3 checks the signature and every file before swapping, swaps all three files together,
  and rolls back if anything goes wrong.
- It never signs anything itself, and no private key ever sits on the ESP.

### ☠ B00T-TOOLS: the toolbox
- **NVRAM-RESET:** clear NVRAM without hunting for the picker hotkey.
- **SIP:** disable or enable System Integrity Protection.
- **DUMP-DISKMAP / DUMP-SSDT:** save the disk map and ACPI tables to the ESP for debugging.
- **UEFI SHELL:** drop to a shell when you need to get your hands dirty.

### ☠ UI DESIGN
Red OTR logo, with a compact layout for smaller screens. A status bar shows the SIP level, current
boot-args and active config at a glance.

## How it works

- **CONFIGS**: lists every `*.plist` in `EFI\OC\B00tGeni3\` (up to 9, `known-good.plist` first) and marks the
  one that matches the live `config.plist` as ACTIVE.
- **Swap**: pick a variant, the pre-flight checks it, you confirm once, it is swapped in, then a cold reset.
  1. The current config is backed up to `B00tGeni3\config.plist.bak`.
  2. The new one is written to `config.tmp` and read back.
  3. Renames put it in place, and it is read back again.
  4. There is always a complete config on disk.
- **RESTORE-FAILSAFE**: one key back to `known-good.plist`.
- **Pre-flight**: a variant is refused, with every reason shown, unless all of these hold:
  - OpenCore's own parser accepts it.
  - `ShowPicker` is true.
  - `B00tGeni3.efi` is still an enabled Tools entry.
  - Every enabled Drivers / Kexts / ACPI / Tools file exists.
  - The vault check passes (see below).

  A config that fails any of these could stop OpenCore before the picker, where this tool can't be
  reached to switch back.
- **BOOT-ARGS**: toggles for `-v -x -s debug=0x100`, a list of documented boot-args with short descriptions,
  and a free edit line.
  - **COMMIT** saves the line to `config.plist`.
  - **APPLY & REBOOT** saves it and cold resets.
  - **B00T ONCE** only sets NVRAM `boot-args` and goes back to the picker. Whether it really lasts
    one boot depends on your config's NVRAM > Delete.
- **B00T-TOOLS**: NVRAM reset, SIP toggle, disk map dump, SSDT dump, UEFI Shell.

## Vault (0.8g)

OpenCore checks every file it loads against `EFI\OC\vault.plist` whenever that file exists, even with
`Vault = Optional`. If a key is patched into `OpenCore.efi`, it also needs `vault.sig` to match. Any
mismatch stops OpenCore **before the picker**.

So a variant that needs a vault brings its own set, signed on your Mac:

```
EFI\OC\B00tGeni3\NAME.plist   the config
EFI\OC\B00tGeni3\NAME.vault   vault.plist made for that config
EFI\OC\B00tGeni3\NAME.vsig    vault.sig for NAME.vault
```

The tool **never signs**.

**Pre-flight checks for a variant's set:**
- the set's signature against the key in `OpenCore.efi`
- that its `config.plist` entry matches the variant
- every file under `EFI\OC` against the vault, both ways

**Swap with a set:**
1. It backs up `config.plist`, `vault.plist` and `vault.sig`.
2. It writes all three as temp files and verifies them.
3. It renames them into place, and rolls back on error.

### Signing on the Mac: `tools/vault-sign.py`

OpenCore's `sign.command` makes a new key on every run, so it can only ever sign one config. This
helper keeps one key and signs every variant with it:

```sh
python3 tools/vault-sign.py keygen ~/b00tgeni3-key /path/to/RsaTool   # once; keep the private key OFF the ESP
python3 tools/vault-sign.py embed OpenCore.efi OpenCore-signed.efi ~/b00tgeni3-key/vault-pub528.bin
python3 tools/vault-sign.py sign  /Volumes/EFI/EFI/OC ~/b00tgeni3-key   # after ANY change under EFI\OC
python3 tools/vault-sign.py check /Volumes/EFI/EFI/OC ~/b00tgeni3-key
```

Keep these in mind:
- **Any change to any file under `EFI\OC` makes every set stale, `known-good` included.** That means
  a kext update, a new tool build, or a Resources change. Until you re-sign, the tool refuses them all.
- **With a vault active, COMMIT and APPLY & REBOOT are refused**, because an edited config can't be
  signed on the ESP. B00T ONCE still works.
- **A power cut during the three renames can stop OpenCore before the picker.** Recovery then needs
  a USB installer.

## Build

Built as an OpenCorePkg application (tested with OpenCorePkg 1.0.8, XCODE5 toolchain, X64):

1. Copy `Application/B00tGeni3/` into an OpenCorePkg 1.0.8 tree.
2. Apply `patches/OpenCorePkg.dsc-b00tgeni3.diff`, which adds one line to `OpenCorePkg.dsc`.
3. Build the module:
   ```sh
   cd OpenCorePkg/UDK && source edksetup.sh
   build -a X64 -b RELEASE -t XCODE5 -p OpenCorePkg/OpenCorePkg.dsc -m OpenCorePkg/Application/B00tGeni3/B00tGeni3.inf
   ```
4. The output is `UDK/Build/OpenCorePkg/RELEASE_XCODE5/X64/B00tGeni3.efi`.

## Install

1. Copy `B00tGeni3.efi` to `EFI\OC\Tools\`.
2. Add a Misc > Tools entry for it (Path `B00tGeni3.efi`, Enabled, `Auxiliary` as you like) to every
   config you will swap between.
3. Put the variants in `EFI\OC\B00tGeni3\`, with `known-good.plist` being one you know boots.
4. Keep a USB installer or a second EFI that boots.

## Bench (`bench/`)

These are the QEMU scripts the gates were run with: OVMF from MacPorts qemu, raw FAT32 ESP images, and
keys and screenshots sent through the QEMU monitor. They assume the original workspace layout (master
ESP folders `bench/ESP` and `bench/ESP-vault`, OpenCorePkg build under `phase0/`), so treat them as a
reference. `bench/CHANGES.txt` lists every build with its sha256.

## Docs

- `docs/evidence/`: gate evidence for each phase (bench runs, screenshots named there, fault tests).
- `docs/phase7-vault-design.md`: what OpenCore does with the vault (read from the 1.0.8 source) and the design.
- `docs/phase8-spec.md`: the UI and feature spec.
- `docs/drafts/`: the A2 screen design, plus vault UI drafts V1 and V1b. These are not built yet.

## What has been tested

- Real hardware: multiple systems, plain and vault setups, run by the author.
- Bench (QEMU + OVMF, OpenCore 1.0.8) results:
  - Plain swaps: 10/10 on 0.8e, 2/2 again on 0.8g. Each one was checked from the host afterwards (disk
    check, file hashes, backup, leftover temp files).
  - Bad variants (empty, garbage, cut off, not a config, Vault Secure, missing kext, over 1 MB): all
    refused, each with its reason.
  - Power cut at 9 points during a swap: `config.plist` was always a whole old or new file, and the next
    swap cleaned up.
  - Vault: a signed swap round trip worked. The tool refused 4 tampered sets, and OpenCore refused the
    same sets. COMMIT is refused while a vault is active. See `docs/evidence/phase7a/7b`.

## Known gaps

- A power cut mid-swap can leave FAT damage or temp files. The next swap clears the temp files; use
  Disk Utility First Aid for the FAT damage. With a vault, a cut during the final renames can stop
  OpenCore before the picker.

## Issues

Found a problem? Open an issue and include:
- the OpenCore version
- the firmware or board
- what the screen showed (a photo is fine)
- the variant that caused it, with serials removed

Fixes come as updates when problems can be pinned down.

## License

BSD 3-Clause, see `LICENSE`. Built on [OpenCorePkg](https://github.com/acidanthera/OpenCorePkg) (also BSD-3).
The notes in `docs/` and `bench/` mention the original build host only in generic terms.
