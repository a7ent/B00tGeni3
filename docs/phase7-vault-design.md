# B00tGeni3 Phase 7 — vault design note (2026-11-17)

Machine: macOS 12.7.6 build host. Sources read this session (OpenCorePkg 1.0.8 tree in workspace/phase0):
Docs/Configuration.tex 4710-4780, Utilities/CreateVault/{create_vault.sh,sign.command}, Utilities/RsaTool/RsaTool.c,
Library/OcStorageLib/OcStorageLib.c, Library/OcMainLib/{OpenCoreVault.c,OpenCoreMisc.c}, Application/OpenCore/OpenCore.c,
Include/Acidanthera/Library/OcCryptoLib.h.

## What OpenCore does (verified in source)
- `vault.plist` = SHA-256 of files under EFI\OC (create_vault.sh: every file except dotfiles, `vault.*`,
  `OpenCore.efi`, MemTest86 logs). Every file OpenCore reads through its storage is checked:
  not in the vault -> "OCST: Aborting %s file access not present in vault"; hash differs -> "Aborting corrupted".
- Files OpenCore never reads (EFI\OC\B00tGeni3\*) are never checked.
- `vault.plist` is used whenever it is present, even with Vault = Optional (Configuration.tex Note 2).
- If OpenCore.efi has a key patched in (528 bytes after `=BEGIN OC VAULT=`), `vault.sig` is required whatever
  Vault says. Bad signature / missing signature / config not loadable / Vault>=Basic without vault /
  Secure without key -> CpuDeadLoop: the machine stops BEFORE the picker. Recovery = USB or another ESP.
- Signature: RSA-2048 PKCS#1 v1.5 over SHA-256 of vault.plist, exponent 65537.

## Why not stock sign.command
RsaTool -sign generates a NEW key on every run and discards the private key (RsaTool.c sign_file), and
sign.command re-patches OpenCore.efi each time. Only one config could ever be valid. B00tGeni3 needs several
signed configs for one OpenCore.efi, so phase7/tools/vault-sign.py keeps ONE persistent openssl key.
(Also: sign.command keeps the key in /tmp and create_vault.sh writes /tmp/vault_hash.)

## Files per variant (EFI\OC\B00tGeni3)
  NAME.plist   the config (listed by the tool as before)
  NAME.vault   vault.plist made for that config (not `.plist`, so the tool does not list it as a variant)
  NAME.vsig    vault.sig for NAME.vault
The B00tGeni3 folder is left out of every vault (OpenCore never reads it; the tool writes backups there).

## Mac side (vault-sign.py) — the tool never signs
  keygen KEYDIR          once; private key stays OFF the ESP
  embed IN OUT PUB528    once per OpenCore.efi build
  sign OCDIR KEYDIR      after ANY change under EFI\OC: makes every NAME.vault/.vsig + OC\vault.plist/.sig
  check OCDIR KEYDIR     host-side verification of every set

## Consequences oTR-DaRkNeSs must know
- Any change to any file under EFI\OC (kext/driver update, a new B00tGeni3.efi build, Resources) makes EVERY
  set stale, known-good included. Until re-signed on the Mac, the tool refuses all of them — no failsafe swap.
- An edited config (COMMIT / APPLY & REBOOT of boot-args) cannot be signed on the ESP. With a vault active
  the tool refuses those writes. B00T ONCE (NVRAM only) still works.
- A power cut during the 3-file swap can leave a mismatched set -> halt before the picker -> USB recovery.
  oTR-DaRkNeSs accepted power-cut risk on 2026-11-17.

## Tool changes planned (0.8g)
Pre-flight, new checks:
1. Vault on disk: OC\vault.plist present, or key patched into OC\OpenCore.efi -> the variant MUST have a set.
   (Also fixes a latent bug: an unsigned ESP with vault.plist would halt after a plain swap.)
2. Set present -> parse NAME.vault (OpenCore's own parser, OcStorageInitializeVault); config.plist entry ==
   SHA-256 of the variant; every other entry (B00tGeni3\ skipped) hashed on disk by streaming, exact-case path;
   every file OpenCore will load per the config is in the vault.
3. Key in OpenCore.efi -> NAME.vsig must verify against that key (RsaVerifySigDataFromKeyDynalloc).
4. Vault = Secure without a key, or Basic/Secure without a set -> refuse (OpenCore would halt).
Swap: write all temps and verify, then rename config, vault.plist, vault.sig; backups of all three.
Variant without a set on an unsigned ESP that has vault.plist: refuse (default; removing vault.plist is oTR-DaRkNeSs's call).
