# B00tGeni3 Phase 8: UI and features

Source: pasted by oTR-DaRkNeSs on 2026-11-15, verbatim below. Mockup photos: phase8/mockup/mockup-photo-1.png and
mockup-photo-2.png (oTR-DaRkNeSs's 2025 mockup.txt shown in nano; oTR-DaRkNeSs is looking for the original files).
Decision the same day: oTR-DaRkNeSs skips on-device testing (Phase 6) for now and moves on to the other planned
features and the UI. This spec says Phase 8 starts only after the Phase 7 gate passes.

---

Phase 8 of B00tGeni3: UI and features. Start only after the Phase 7 gate passes. Reference: oTR-DaRkNeSs's 2025 mockup (mockup.txt), shown in the two photos oTR-DaRkNeSs is attaching. Keep his layout, art, wording and positions; the name is B00tGeni3 only, never with an OC prefix. Colors: green B00tGeni3 logo, white borders, yellow for the selected item, red for warnings, black background, using only the 16 EFI_TEXT_ATTR colors. Arrow keys and Enter only, no mouse.

8a Glyph and size test. OpenCore's builtin renderer font (Library/OcConsoleLib/ConsoleFont.c) has ISO Latin-1, the EFI box-drawing set, arrows, and only two block elements, U+2588 and U+2591. U+2592, U+2593 and U+2620 are missing. Confirm that in the 1.0.8 source. Then add a test screen to the tool that prints every glyph the mockup uses and the current ConOut columns x rows. Run it on the bench with TextRenderer BuiltinGraphics at 1024x768 and 1920x1080. Gate: screenshots, plus a list of the glyphs that render and the ones that don't.

8b Art. Draw the OTR DARKNSS logo, the big B00TG3N13 logo and the bottom banner as bitmaps embedded in the .efi and drawn with GOP Blt, so the shading matches the mockup. Draw everything else as text. If the console is too small for the full 56-line layout, fall back to a compact layout with a small logo. Gate: screenshots at both resolutions next to the mockup photo.

8c Screen sections. System info row: boot device from the LoadedImage device path; serial from the active config's PlatformInfo Generic SystemSerialNumber; SIP level from NVRAM csr-active-config and current boot-args from NVRAM boot-args, both under GUID 7C436110-AB2A-4BBB-A880-FE41995C9F82; MAC from SimpleNetwork if present, otherwise n/a. CONFIG panel: RESTORE-FAILSAFE and LOAD-ALTERNATE call the existing Phase 4 swap and Phase 5 pre-flight, unchanged. KEXT CONTROL shows as coming soon. BOOT-TOOLS panel: NVRAM-RESET, DISABLE/ENABLE SIP and UEFI SHELL start the existing Tools entries (ResetNvramEntry, ToggleSipEntry, OpenShell) with LoadImage and StartImage instead of reimplementing them. DUMP-DISKMAP lists filesystem and block devices with their device paths. DUMP SSDT writes the ACPI tables from the system configuration table to a dated folder on the ESP, because SysReport is not available in RELEASE builds. DISCARD CHANGES AND EXIT returns to the picker. Gate: screenshot of every section and each tool launch on the bench.

8d Boot-args. Per Configuration.tex, NVRAM Add only sets a variable if it is not present or was deleted, so with boot-args under NVRAM Delete the config value wins every boot. Build two paths. One-shot: write NVRAM boot-args and return to the picker without a reset, so only this boot uses them; label it untested in the UI until a real macOS boot confirms it. COMMIT and APPLY & REBOOT: write the new boot-args string into a copy of the config's NVRAM Add value as a text-level edit of that one string, run the Phase 5 pre-flight on the copy, then swap it in through the Phase 4 path and cold reset. Controls: the -v, -x, -s and debug=0x100 checkboxes, an ADDITIONAL-ARGS-LIST of common args to pick from, and the editable boot-arg line. Gate: on the bench, COMMIT puts the expected string in the swapped config, the pre-flight passes, and a diff of the raw XML shows only the boot-args string changed.

Stop after each sub-gate and report with screenshots. Do not touch any EFI partition outside the bench image, do not push to a7ent/B00tGeni3, and do not reuse any 2025 B00tGeni3 code.

---
Decisions by oTR-DaRkNeSs, 2026-11-15 (asked by CC after reading the vault code):
- Phase 7 is SKIPPED for now. The vault swap is NOT to be built: the 3-file swap (config.plist, vault.plist,
  vault.sig) cannot be atomic on FAT, and a power cut mid-swap leaves a set OpenCore refuses before the picker.
- The tool keeps refusing any variant whose Vault is not Optional.
- Phase 8 therefore starts without the Phase 7 gate, overriding the first line of the spec above.
