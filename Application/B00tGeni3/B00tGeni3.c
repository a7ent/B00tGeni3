/** @file
  B00tGeni3 config picker tool.

  Lists the config variants in EFI\OC\B00tGeni3 (every *.plist there, with
  known-good.plist pinned first), marks the one matching the live
  EFI\OC\config.plist, and on a pick swaps it in and cold resets:

    1. back up config.plist to EFI\OC\B00tGeni3\config.plist.bak
    2. write the variant to EFI\OC\config.tmp and read it back
    3. rename config.plist -> config.old, config.tmp -> config.plist
    4. read config.plist back, delete config.old
    5. record the variant name in EFI\OC\B00tGeni3\active.txt
    6. cold reset

  Before a swap the variant goes through a pre-flight check and is refused,
  with every reason printed, unless: it parses with OcConfigurationLib,
  ShowPicker is true, Vault is Optional, this tool is still an enabled Tools
  entry, and every file referenced by an enabled Drivers, Kexts, ACPI or Tools
  entry exists. A config failing these could stop OpenCore before the picker,
  where this tool cannot be reached to switch back.

  Every write goes to a temporary name first and is read back before it is
  renamed into place, and there is always a complete config on disk
  (config.plist or config.old).

  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Uefi.h>
#include <Guid/AppleVariable.h>
#include <Guid/FileInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleNetwork.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAcpiLib.h>
#include <Library/OcConfigurationLib.h>
#include <Library/OcCryptoLib.h>
#include <Library/OcStringLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "B00tArt.h"

#define B00T_VERSION         L"0.8c"
#define B00T_TOOL_NAME       "B00tGeni3.efi"
#define B00T_MAX_PATH        256
#define B00T_MAX_ARGS        256
#define B00T_APPLE_BOOT_GUID  "7C436110-AB2A-4BBB-A880-FE41995C9F82"
#define B00T_OC_DIR_PATH     L"EFI\\OC"
#define B00T_DIR_PATH        L"EFI\\OC\\B00tGeni3"
#define B00T_CONFIG_NAME     L"config.plist"
#define B00T_CONFIG_TMP      L"config.tmp"
#define B00T_CONFIG_OLD      L"config.old"
#define B00T_BACKUP_NAME     L"config.plist.bak"
#define B00T_BACKUP_TMP      L"config.bak.tmp"
#define B00T_ACTIVE_NAME     L"active.txt"
#define B00T_ACTIVE_TMP      L"active.tmp"
#define B00T_KNOWN_GOOD      L"known-good.plist"
#define B00T_MAX_VARIANTS    9
#define B00T_MAX_NAME        64
#define B00T_MAX_FILE_SIZE   SIZE_1MB

typedef struct {
  CHAR16     Name[B00T_MAX_NAME];
  UINT8      *Data;
  UINTN      Size;
  UINT8      Hash[SHA256_DIGEST_SIZE];
  BOOLEAN    Active;
} B00T_VARIANT;

STATIC
VOID
PrintDevicePath (
  IN CONST CHAR16              *Label,
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  CHAR16  *Text;

  if (DevicePath == NULL) {
    Print (L"%s: <none>\n", Label);
    return;
  }

  Text = ConvertDevicePathToText (DevicePath, FALSE, FALSE);
  if (Text == NULL) {
    Print (L"%s: <cannot convert>\n", Label);
    return;
  }

  Print (L"%s: %s\n", Label, Text);
  DEBUG ((DEBUG_INFO, "B00T: %s: %s\n", Label, Text));
  FreePool (Text);
}

STATIC
EFI_INPUT_KEY
WaitForKey (
  VOID
  )
{
  EFI_INPUT_KEY  Key;
  UINTN          Index;

  //
  // Drop keys already queued, such as the Enter that launched us from the picker.
  //
  while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
  }

  ZeroMem (&Key, sizeof (Key));
  gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
  gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
  return Key;
}

STATIC
VOID
PrintHashPrefix (
  IN CONST UINT8  *Hash
  )
{
  UINTN  Index;

  for (Index = 0; Index < 8; ++Index) {
    Print (L"%02x", Hash[Index]);
  }
}

STATIC
CHAR16
B00tToLower (
  IN CHAR16  Char
  )
{
  if ((Char >= L'A') && (Char <= L'Z')) {
    return (CHAR16)(Char - L'A' + L'a');
  }

  return Char;
}

STATIC
BOOLEAN
B00tStriEqual (
  IN CONST CHAR16  *First,
  IN CONST CHAR16  *Second
  )
{
  while ((*First != L'\0') && (B00tToLower (*First) == B00tToLower (*Second))) {
    ++First;
    ++Second;
  }

  return B00tToLower (*First) == B00tToLower (*Second);
}

STATIC
BOOLEAN
HasPlistExtension (
  IN CONST CHAR16  *Name
  )
{
  UINTN  Length;

  Length = StrLen (Name);
  return Length > 6 && B00tStriEqual (Name + Length - 6, L".plist");
}

STATIC
EFI_STATUS
DeleteIfExists (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *Name
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;

  Status = Dir->Open (Dir, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (EFI_ERROR (Status)) {
    return Status == EFI_NOT_FOUND ? EFI_SUCCESS : Status;
  }

  //
  // Delete closes the handle, and returns a warning if the file could not be deleted.
  //
  Status = File->Delete (File);
  return Status == EFI_SUCCESS ? EFI_SUCCESS : EFI_ACCESS_DENIED;
}

STATIC
EFI_STATUS
ReadWholeFile (
  IN  EFI_FILE_PROTOCOL  *Dir,
  IN  CONST CHAR16       *Name,
  OUT UINT8              **Data,
  OUT UINTN              *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINTN              ReadSize;
  UINT8              *Buffer;

  *Data = NULL;
  *Size = 0;

  Status = Dir->Open (Dir, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    File->Close (File);
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status) || (Info->FileSize > B00T_MAX_FILE_SIZE)) {
    FreePool (Info);
    File->Close (File);
    return EFI_ERROR (Status) ? Status : EFI_BAD_BUFFER_SIZE;
  }

  ReadSize = (UINTN)Info->FileSize;
  FreePool (Info);

  //
  // One spare byte so an empty file still gets a valid buffer.
  //
  Buffer = AllocatePool (ReadSize + 1);
  if (Buffer == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  *Size  = ReadSize;
  Status = File->Read (File, &ReadSize, Buffer);
  File->Close (File);
  if (EFI_ERROR (Status) || (ReadSize != *Size)) {
    FreePool (Buffer);
    *Size = 0;
    return EFI_ERROR (Status) ? Status : EFI_END_OF_FILE;
  }

  *Data = Buffer;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
WriteWholeFile (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *Name,
  IN CONST UINT8        *Data,
  IN UINTN              Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINTN              WriteSize;

  //
  // Remove any leftover so the new file is written from offset 0 with exact size.
  //
  Status = DeleteIfExists (Dir, Name);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Dir->Open (
                  Dir,
                  &File,
                  (CHAR16 *)Name,
                  EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                  0
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  WriteSize = Size;
  Status    = File->Write (File, &WriteSize, (VOID *)Data);
  if (!EFI_ERROR (Status) && (WriteSize != Size)) {
    Status = EFI_VOLUME_FULL;
  }

  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }

  File->Close (File);
  return Status;
}

STATIC
EFI_STATUS
ReadAndCompare (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *Name,
  IN CONST UINT8        *Expected,
  IN UINTN              Size
  )
{
  EFI_STATUS  Status;
  UINT8       *Data;
  UINTN       DataSize;

  Status = ReadWholeFile (Dir, Name, &Data, &DataSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((DataSize != Size) || (CompareMem (Data, Expected, Size) != 0)) {
    Status = EFI_CRC_ERROR;
  }

  FreePool (Data);
  return Status;
}

STATIC
EFI_STATUS
RenameNoProbe (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *OldName,
  IN CONST CHAR16       *NewName
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  EFI_FILE_INFO      *NewInfo;
  UINTN              InfoSize;
  UINTN              NewInfoSize;

  Status = Dir->Open (Dir, &File, (CHAR16 *)OldName, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    File->Close (File);
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    FreePool (Info);
    File->Close (File);
    return Status;
  }

  NewInfoSize = SIZE_OF_EFI_FILE_INFO + StrSize (NewName);
  NewInfo     = AllocateZeroPool (NewInfoSize);
  if (NewInfo == NULL) {
    FreePool (Info);
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (NewInfo, Info, SIZE_OF_EFI_FILE_INFO);
  NewInfo->Size = NewInfoSize;
  StrCpyS (NewInfo->FileName, StrSize (NewName) / sizeof (CHAR16), NewName);

  Status = File->SetInfo (File, &gEfiFileInfoGuid, NewInfoSize, NewInfo);
  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }

  FreePool (NewInfo);
  FreePool (Info);
  File->Close (File);
  return Status;
}

STATIC
EFI_STATUS
RenameFile (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *OldName,
  IN CONST CHAR16       *NewName
  )
{
  EFI_STATUS  Status;

  //
  // Drop an existing target first; FAT will not rename over it.
  //
  Status = DeleteIfExists (Dir, NewName);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return RenameNoProbe (Dir, OldName, NewName);
}

/**
  Write Data to TmpName, read it back, then rename it to FinalName.
**/
STATIC
EFI_STATUS
WriteViaTemp (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *TmpName,
  IN CONST CHAR16       *FinalName,
  IN CONST UINT8        *Data,
  IN UINTN              Size
  )
{
  EFI_STATUS  Status;

  Status = WriteWholeFile (Dir, TmpName, Data, Size);
  if (!EFI_ERROR (Status)) {
    Status = ReadAndCompare (Dir, TmpName, Data, Size);
  }

  if (!EFI_ERROR (Status)) {
    Status = RenameFile (Dir, TmpName, FinalName);
  }

  if (!EFI_ERROR (Status)) {
    Status = ReadAndCompare (Dir, FinalName, Data, Size);
  }

  return Status;
}

STATIC
VOID
FreeVariants (
  IN B00T_VARIANT  *Variants,
  IN UINTN         Count
  )
{
  UINTN  Index;

  for (Index = 0; Index < Count; ++Index) {
    if (Variants[Index].Data != NULL) {
      FreePool (Variants[Index].Data);
      Variants[Index].Data = NULL;
    }
  }
}

/**
  Sort order: known-good.plist first, then by name.
**/
STATIC
BOOLEAN
VariantBefore (
  IN CONST B00T_VARIANT  *First,
  IN CONST B00T_VARIANT  *Second
  )
{
  if (B00tStriEqual (First->Name, B00T_KNOWN_GOOD)) {
    return TRUE;
  }

  if (B00tStriEqual (Second->Name, B00T_KNOWN_GOOD)) {
    return FALSE;
  }

  return StrCmp (First->Name, Second->Name) < 0;
}

STATIC
EFI_STATUS
LoadVariants (
  IN  EFI_FILE_PROTOCOL  *B00tDir,
  IN  CONST UINT8        *ConfigData,
  IN  UINTN              ConfigSize,
  OUT B00T_VARIANT       *Variants,
  OUT UINTN              *Count
  )
{
  EFI_STATUS     Status;
  EFI_FILE_INFO  *Info;
  UINTN          InfoSize;
  UINTN          ReadSize;
  B00T_VARIANT   Swap;
  UINTN          Index;
  UINTN          Inner;

  *Count   = 0;
  InfoSize = SIZE_OF_EFI_FILE_INFO + B00T_MAX_NAME * sizeof (CHAR16) * 4;
  Info     = AllocatePool (InfoSize);
  if (Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = B00tDir->SetPosition (B00tDir, 0);
  while (!EFI_ERROR (Status)) {
    ReadSize = InfoSize;
    Status   = B00tDir->Read (B00tDir, &ReadSize, Info);
    if (EFI_ERROR (Status) || (ReadSize == 0)) {
      break;
    }

    if (((Info->Attribute & EFI_FILE_DIRECTORY) != 0)
        || !HasPlistExtension (Info->FileName)
        || (StrLen (Info->FileName) >= B00T_MAX_NAME))
    {
      continue;
    }

    if (*Count == B00T_MAX_VARIANTS) {
      Print (L"More than %u variants - ignoring %s\n", B00T_MAX_VARIANTS, Info->FileName);
      continue;
    }

    ZeroMem (&Variants[*Count], sizeof (Variants[*Count]));
    StrCpyS (Variants[*Count].Name, B00T_MAX_NAME, Info->FileName);
    Status = ReadWholeFile (B00tDir, Info->FileName, &Variants[*Count].Data, &Variants[*Count].Size);
    if (EFI_ERROR (Status)) {
      Print (L"Cannot read %s - %r\n", Info->FileName, Status);
      break;
    }

    Sha256 (Variants[*Count].Hash, Variants[*Count].Data, Variants[*Count].Size);
    Variants[*Count].Active = (ConfigData != NULL)
                              && (Variants[*Count].Size == ConfigSize)
                              && (CompareMem (Variants[*Count].Data, ConfigData, ConfigSize) == 0);
    ++(*Count);
  }

  FreePool (Info);
  if (EFI_ERROR (Status)) {
    FreeVariants (Variants, *Count);
    *Count = 0;
    return Status;
  }

  for (Index = 1; Index < *Count; ++Index) {
    for (Inner = Index; Inner > 0 && VariantBefore (&Variants[Inner], &Variants[Inner - 1]); --Inner) {
      CopyMem (&Swap, &Variants[Inner], sizeof (Swap));
      CopyMem (&Variants[Inner], &Variants[Inner - 1], sizeof (Swap));
      CopyMem (&Variants[Inner - 1], &Swap, sizeof (Swap));
    }
  }

  return EFI_SUCCESS;
}

//
// Parsed copy of the variant under pre-flight. Static: OC_GLOBAL_CONFIG is too large for the stack.
//
STATIC OC_GLOBAL_CONFIG  mPreflightConfig;

STATIC
VOID
PreflightFile (
  IN     EFI_FILE_PROTOCOL  *OcDir,
  IN     CONST CHAR16       *Folder,
  IN     CONST CHAR8        *Part1,
  IN     CONST CHAR8        *Part2  OPTIONAL,
  IN OUT UINTN              *Problems
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  CHAR16             Path[B00T_MAX_PATH];

  if (Part2 != NULL) {
    Status = OcUnicodeSafeSPrint (Path, sizeof (Path), L"%s\\%a\\%a", Folder, Part1, Part2);
  } else {
    Status = OcUnicodeSafeSPrint (Path, sizeof (Path), L"%s\\%a", Folder, Part1);
  }

  if (EFI_ERROR (Status)) {
    Print (L"  - path too long: %s\\%a\n", Folder, Part1);
    ++*Problems;
    return;
  }

  UnicodeUefiSlashes (Path);
  Status = OcDir->Open (OcDir, &File, Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Print (L"  - missing file: EFI\\OC\\%s (%r)\n", Path, Status);
    ++*Problems;
    return;
  }

  File->Close (File);
}

/**
  Check that a variant would still bring OpenCore to its picker with this tool in it.
  Prints one line per problem. Changes nothing on disk.

  @retval TRUE   No problems found, the variant may be activated.
  @retval FALSE  At least one problem, the variant must be refused.
**/
STATIC
BOOLEAN
PreflightVariant (
  IN EFI_FILE_PROTOCOL   *OcDir,
  IN CONST B00T_VARIANT  *Variant
  )
{
  EFI_STATUS           Status;
  OC_GLOBAL_CONFIG     *Config;
  UINT8                *Copy;
  UINT32               ErrorCount;
  UINTN                Problems;
  UINT32               Index;
  BOOLEAN              ToolListed;
  OC_MISC_TOOLS_ENTRY  *Tool;
  OC_UEFI_DRIVER_ENTRY *Driver;
  OC_KERNEL_ADD_ENTRY  *Kext;
  OC_ACPI_ADD_ENTRY    *Table;
  CONST CHAR8          *Path;

  //
  // The plist parser works in place, so it gets a copy: the variant data is what gets written.
  //
  Copy = AllocateCopyPool (Variant->Size, Variant->Data);
  if (Copy == NULL) {
    Print (L"  - out of memory, cannot check\n");
    return FALSE;
  }

  Config     = &mPreflightConfig;
  ErrorCount = 0;
  Status     = OcConfigurationInit (Config, Copy, (UINT32)Variant->Size, &ErrorCount);
  if (EFI_ERROR (Status)) {
    Print (L"  - not a valid plist: OpenCore's parser rejects it (%r)\n", Status);
    FreePool (Copy);
    return FALSE;
  }

  Problems = 0;
  if (ErrorCount > 0) {
    Print (L"  note: %u schema warning(s), OpenCore would log them and continue\n", ErrorCount);
  }

  if (!Config->Misc.Boot.ShowPicker) {
    Print (L"  - Misc > Boot > ShowPicker is false: the picker would not appear\n");
    ++Problems;
  }

  Path = OC_BLOB_GET (&Config->Misc.Security.Vault);
  if (AsciiStrCmp (Path, "Optional") != 0) {
    Print (L"  - Misc > Security > Vault is %a: needs Optional (vault support is not in this version)\n", Path);
    ++Problems;
  }

  ToolListed = FALSE;
  for (Index = 0; Index < Config->Misc.Tools.Count; ++Index) {
    Tool = Config->Misc.Tools.Values[Index];
    Path = OC_BLOB_GET (&Tool->Path);
    if (!Tool->Enabled || (Path[0] == '\0')) {
      continue;
    }

    if (AsciiStriCmp (Path, B00T_TOOL_NAME) == 0) {
      ToolListed = TRUE;
    }

    PreflightFile (OcDir, L"Tools", Path, NULL, &Problems);
  }

  if (!ToolListed) {
    Print (L"  - %a is not an enabled entry under Misc > Tools: no way back from this config\n", B00T_TOOL_NAME);
    ++Problems;
  }

  for (Index = 0; Index < Config->Uefi.Drivers.Count; ++Index) {
    Driver = Config->Uefi.Drivers.Values[Index];
    Path   = OC_BLOB_GET (&Driver->Path);
    if (Driver->Enabled && (Path[0] != '\0')) {
      PreflightFile (OcDir, L"Drivers", Path, NULL, &Problems);
    }
  }

  for (Index = 0; Index < Config->Kernel.Add.Count; ++Index) {
    Kext = Config->Kernel.Add.Values[Index];
    Path = OC_BLOB_GET (&Kext->BundlePath);
    if (!Kext->Enabled || (Path[0] == '\0') || (OC_BLOB_GET (&Kext->PlistPath)[0] == '\0')) {
      continue;
    }

    PreflightFile (OcDir, L"Kexts", Path, OC_BLOB_GET (&Kext->PlistPath), &Problems);
    if (OC_BLOB_GET (&Kext->ExecutablePath)[0] != '\0') {
      PreflightFile (OcDir, L"Kexts", Path, OC_BLOB_GET (&Kext->ExecutablePath), &Problems);
    }
  }

  for (Index = 0; Index < Config->Acpi.Add.Count; ++Index) {
    Table = Config->Acpi.Add.Values[Index];
    Path  = OC_BLOB_GET (&Table->Path);
    if (Table->Enabled && (Path[0] != '\0')) {
      PreflightFile (OcDir, L"ACPI", Path, NULL, &Problems);
    }
  }

  OcConfigurationFree (Config);
  FreePool (Copy);

  if (Problems == 0) {
    Print (L"  OK\n");
  }

  return Problems == 0;
}

STATIC
EFI_STATUS
SwapConfig (
  IN EFI_FILE_PROTOCOL   *OcDir,
  IN EFI_FILE_PROTOCOL   *B00tDir,
  IN CONST UINT8         *ConfigData,
  IN UINTN               ConfigSize,
  IN CONST B00T_VARIANT  *Variant
  )
{
  EFI_STATUS  Status;
  CHAR8       ActiveText[B00T_MAX_NAME + 2];

  Status = WriteViaTemp (B00tDir, B00T_BACKUP_TMP, B00T_BACKUP_NAME, ConfigData, ConfigSize);
  Print (L"Back up config.plist -> B00tGeni3\\%s - %r\n", B00T_BACKUP_NAME, Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = WriteWholeFile (OcDir, B00T_CONFIG_TMP, Variant->Data, Variant->Size);
  if (!EFI_ERROR (Status)) {
    Status = ReadAndCompare (OcDir, B00T_CONFIG_TMP, Variant->Data, Variant->Size);
  }

  Print (L"Write %s to OC\\%s - %r\n", Variant->Name, B00T_CONFIG_TMP, Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = RenameFile (OcDir, B00T_CONFIG_NAME, B00T_CONFIG_OLD);
  Print (L"Rename config.plist -> %s - %r\n", B00T_CONFIG_OLD, Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = RenameFile (OcDir, B00T_CONFIG_TMP, B00T_CONFIG_NAME);
  if (!EFI_ERROR (Status)) {
    Status = ReadAndCompare (OcDir, B00T_CONFIG_NAME, Variant->Data, Variant->Size);
  }

  Print (L"Rename %s -> config.plist and verify - %r\n", B00T_CONFIG_TMP, Status);
  if (EFI_ERROR (Status)) {
    //
    // Put the previous config back so OpenCore still has one to load.
    //
    Print (L"Restoring previous config.plist - %r\n", RenameFile (OcDir, B00T_CONFIG_OLD, B00T_CONFIG_NAME));
    return Status;
  }

  Status = DeleteIfExists (OcDir, B00T_CONFIG_OLD);
  Print (L"Delete %s - %r\n", B00T_CONFIG_OLD, Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UnicodeStrToAsciiStrS (Variant->Name, ActiveText, B00T_MAX_NAME);
  AsciiStrCatS (ActiveText, sizeof (ActiveText), "\n");
  Status = WriteViaTemp (B00tDir, B00T_ACTIVE_TMP, B00T_ACTIVE_NAME, (UINT8 *)ActiveText, AsciiStrLen (ActiveText));
  Print (L"Record active name in B00tGeni3\\%s - %r\n", B00T_ACTIVE_NAME, Status);
  return Status;
}

//
// Glyph and size test (menu key G). Phase 8a: shows which characters the active text renderer
// can draw and how many columns and rows the console has. Changes nothing on disk.
//
typedef struct {
  CHAR16          Char;
  CONST CHAR16    *Name;
} B00T_GLYPH;

STATIC CONST B00T_GLYPH  mMockupGlyphs[] = {
  { 0x2588, L"FULL BLOCK"           },
  { 0x2593, L"DARK SHADE"           },
  { 0x2592, L"MEDIUM SHADE"         },
  { 0x2591, L"LIGHT SHADE"          },
  { 0x2620, L"SKULL AND CROSSBONES" },
  { 0x25B2, L"UP TRIANGLE"          },
  { 0x25BC, L"DOWN TRIANGLE"        },
  { 0x25BA, L"RIGHT POINTER"        },
  { 0x25C4, L"LEFT POINTER"         },
  { L'*',   L"ASTERISK"             },
  { L'#',   L"NUMBER SIGN"          },
  { L'=',   L"EQUALS"               },
  { L'~',   L"TILDE"                },
};

STATIC CONST CHAR16  *mColorNames[] = {
  L"BLACK",    L"BLUE",      L"GREEN",      L"CYAN",      L"RED",      L"MAGENTA",      L"BROWN",  L"LIGHTGRAY",
  L"DARKGRAY", L"LIGHTBLUE", L"LIGHTGREEN", L"LIGHTCYAN", L"LIGHTRED", L"LIGHTMAGENTA", L"YELLOW", L"WHITE"
};

STATIC
VOID
GlyphRange (
  IN CONST CHAR16  *Label,
  IN CHAR16        First,
  IN CHAR16        Last
  )
{
  CHAR16  Line[32 * 2 + 1];
  CHAR16  Char;
  UINTN   Count;

  Print (L"%s\n", Label);
  Char = First;
  while (Char <= Last) {
    Print (L"  U+%04X  ", Char);
    for (Count = 0; Count < 32 && Char <= Last; ++Count, ++Char) {
      Line[Count * 2]     = Char;
      Line[Count * 2 + 1] = L' ';
    }

    Line[Count * 2] = L'\0';
    gST->ConOut->OutputString (gST->ConOut, Line);
    Print (L"\n");
  }
}

STATIC
VOID
GlyphTest (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  UINTN                         Columns;
  UINTN                         Rows;
  UINTN                         Index;
  CHAR16                        One[2];

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"B00tGeni3 %s - glyph and size test\n", B00T_VERSION);

  Status = gST->ConOut->QueryMode (gST->ConOut, (UINTN)gST->ConOut->Mode->Mode, &Columns, &Rows);
  if (EFI_ERROR (Status)) {
    Print (L"ConOut mode %d: size unknown - %r\n", gST->ConOut->Mode->Mode, Status);
  } else {
    Print (L"ConOut mode %d of %d: %u columns x %u rows\n", gST->ConOut->Mode->Mode, gST->ConOut->Mode->MaxMode, (UINT32)Columns, (UINT32)Rows);
  }

  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  }

  if (EFI_ERROR (Status)) {
    Print (L"GOP: not available - %r\n", Status);
  } else {
    Print (L"GOP: %u x %u pixels\n", Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution);
  }

  Print (L"\nGlyphs the mockup uses (the glyph is between the brackets):\n");
  One[1] = L'\0';
  for (Index = 0; Index < ARRAY_SIZE (mMockupGlyphs); ++Index) {
    One[0] = mMockupGlyphs[Index].Char;
    Print (L"  U+%04X [", mMockupGlyphs[Index].Char);
    gST->ConOut->OutputString (gST->ConOut, One);
    Print (L"] %s\n", mMockupGlyphs[Index].Name);
  }

  Print (L"\n");
  GlyphRange (L"ASCII", 0x20, 0x7E);
  GlyphRange (L"Latin-1", 0xA0, 0xFF);
  GlyphRange (L"Arrows", 0x2190, 0x2199);
  GlyphRange (L"Box drawing", 0x2500, 0x257F);
  GlyphRange (L"Block elements", 0x2580, 0x259F);
  GlyphRange (L"Geometric shapes", 0x25A0, 0x25CF);

  Print (L"\nColors (EFI_TEXT_ATTR foreground 0-15 on black):\n  ");
  for (Index = 0; Index < ARRAY_SIZE (mColorNames); ++Index) {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (Index, Index == EFI_BLACK ? EFI_LIGHTGRAY : EFI_BLACK));
    Print (L"%s", mColorNames[Index]);
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
    Print (Index == 7 ? L"\n  " : L" ");
  }

  Print (L"\n\nPress any key for the menu...\n");
}

//
// Mockup screen. Text goes through ConOut; the logos and the banner are bitmaps
// drawn with GOP Blt at the pixel positions of the builtin renderer's cells.
//
typedef struct {
  EFI_GRAPHICS_OUTPUT_PROTOCOL    *Gop;
  UINTN                           Columns;
  UINTN                           Rows;
  UINTN                           Shift;
  UINTN                           StatusRow;
  UINTN                           Scale;
  UINTN                           CellWidth;
  UINTN                           CellHeight;
  UINTN                           PadX;
  UINTN                           PadY;
  UINTN                           Col0;
  UINTN                           Row0;
  BOOLEAN                         Compact;
} B00T_UI;

STATIC B00T_UI  mUi;

STATIC CONST UINT8  mShadeLevels[] = { 0, 74, 145, 194, 255 };

/**
  Work out whether the mockup screen fits this console and where its cells are in pixels.
  The geometry is the one of OpenCore's builtin renderer (8x16 cells times the UI scale,
  centred); any other console gets the plain menu.
**/
STATIC
BOOLEAN
UiInit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Columns;
  UINTN       Rows;
  UINTN       Width;
  UINTN       Height;

  Status = gST->ConOut->QueryMode (gST->ConOut, (UINTN)gST->ConOut->Mode->Mode, &Columns, &Rows);
  if (EFI_ERROR (Status) || (Columns == 0) || (Rows == 0)) {
    return FALSE;
  }

  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&mUi.Gop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mUi.Gop);
  }

  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Width          = mUi.Gop->Mode->Info->HorizontalResolution;
  Height         = mUi.Gop->Mode->Info->VerticalResolution;
  mUi.Scale      = (Width / Columns) >= 16 ? 2 : 1;
  mUi.CellWidth  = 8 * mUi.Scale;
  mUi.CellHeight = 16 * mUi.Scale;
  if (  (Columns * mUi.CellWidth > Width)
     || (Rows * mUi.CellHeight > Height)
     || (Width - Columns * mUi.CellWidth >= mUi.CellWidth)
     || (Height - Rows * mUi.CellHeight >= mUi.CellHeight))
  {
    return FALSE;
  }

  if ((Columns < B00T_UI_COLS) || (Rows < B00T_UI_COMPACT_ROWS)) {
    return FALSE;
  }

  mUi.Compact   = Rows < B00T_UI_ROWS;
  mUi.Columns   = Columns;
  mUi.Rows      = Rows;
  mUi.Shift     = mUi.Compact ? B00T_UI_ROWS - B00T_UI_COMPACT_ROWS : 0;
  mUi.StatusRow = (mUi.Compact ? B00T_UI_COMPACT_ROWS : B00T_UI_ROWS) - 1;
  mUi.PadX    = (Width - Columns * mUi.CellWidth) / 2;
  mUi.PadY    = (Height - Rows * mUi.CellHeight) / 2;
  mUi.Col0    = (Columns - B00T_UI_COLS) / 2;
  mUi.Row0    = (Rows - (mUi.Compact ? B00T_UI_COMPACT_ROWS : B00T_UI_ROWS)) / 2;
  return TRUE;
}

/**
  Black out the whole display. ClearScreen alone only clears the area text has reached,
  which leaves bitmaps drawn outside it.
**/
STATIC
VOID
UiBlackout (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;

  ZeroMem (&Black, sizeof (Black));
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
  gST->ConOut->ClearScreen (gST->ConOut);
  mUi.Gop->Blt (
             mUi.Gop,
             &Black,
             EfiBltVideoFill,
             0,
             0,
             0,
             0,
             mUi.Gop->Mode->Info->HorizontalResolution,
             mUi.Gop->Mode->Info->VerticalResolution,
             0
             );
}

/**
  Draw a logo given as one shade level per cell. Cells are full character cells, or
  half size in the compact layout; the logo is centred in ColSpan x RowSpan cells.
**/
STATIC
VOID
UiDrawCells (
  IN CONST UINT8  *Cells,
  IN UINTN        Cols,
  IN UINTN        Rows,
  IN UINTN        Col,
  IN UINTN        Row,
  IN UINTN        RowSpan,
  IN UINT8        Red,
  IN UINT8        Green,
  IN UINT8        Blue
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Buffer;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixel;
  UINTN                          BlockWidth;
  UINTN                          BlockHeight;
  UINTN                          Width;
  UINTN                          Height;
  UINTN                          X;
  UINTN                          Y;
  UINT8                          Shade;

  BlockWidth  = mUi.Compact ? mUi.CellWidth / 2 : mUi.CellWidth;
  BlockHeight = mUi.Compact ? mUi.CellHeight / 2 : mUi.CellHeight;
  Width       = Cols * BlockWidth;
  Height      = Rows * BlockHeight;
  Buffer      = AllocatePool (Width * Height * sizeof (*Buffer));
  if (Buffer == NULL) {
    return;
  }

  Pixel = Buffer;
  for (Y = 0; Y < Height; ++Y) {
    for (X = 0; X < Width; ++X) {
      Shade           = mShadeLevels[Cells[(Y / BlockHeight) * Cols + X / BlockWidth]];
      Pixel->Red      = (UINT8)((UINTN)Red * Shade / 255);
      Pixel->Green    = (UINT8)((UINTN)Green * Shade / 255);
      Pixel->Blue     = (UINT8)((UINTN)Blue * Shade / 255);
      Pixel->Reserved = 0;
      ++Pixel;
    }
  }

  mUi.Gop->Blt (
             mUi.Gop,
             Buffer,
             EfiBltBufferToVideo,
             0,
             0,
             mUi.PadX + (mUi.Col0 + Col) * mUi.CellWidth + (Cols * mUi.CellWidth - Width) / 2,
             mUi.PadY + (mUi.Row0 + Row) * mUi.CellHeight + (RowSpan * mUi.CellHeight - Height) / 2,
             Width,
             Height,
             0
             );
  FreePool (Buffer);
}

/**
  Draw a 1 bit per pixel bitmap (most significant bit first, rows padded to a byte)
  in white with its top left corner on a cell.
**/
STATIC
VOID
UiDrawBits (
  IN CONST UINT8  *Bits,
  IN UINTN        BitsWidth,
  IN UINTN        BitsHeight,
  IN UINTN        Col,
  IN UINTN        Row
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Buffer;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixel;
  UINTN                          Stride;
  UINTN                          Width;
  UINTN                          Height;
  UINTN                          X;
  UINTN                          Y;
  UINTN                          SourceX;
  UINT8                          Value;

  Stride = (BitsWidth + 7) / 8;
  Width  = BitsWidth * mUi.Scale;
  Height = BitsHeight * mUi.Scale;
  Buffer = AllocatePool (Width * Height * sizeof (*Buffer));
  if (Buffer == NULL) {
    return;
  }

  Pixel = Buffer;
  for (Y = 0; Y < Height; ++Y) {
    for (X = 0; X < Width; ++X) {
      SourceX         = X / mUi.Scale;
      Value           = (Bits[(Y / mUi.Scale) * Stride + SourceX / 8] & (0x80 >> (SourceX % 8))) != 0 ? 0xFF : 0;
      Pixel->Red      = Value;
      Pixel->Green    = Value;
      Pixel->Blue     = Value;
      Pixel->Reserved = 0;
      ++Pixel;
    }
  }

  mUi.Gop->Blt (
             mUi.Gop,
             Buffer,
             EfiBltBufferToVideo,
             0,
             0,
             mUi.PadX + (mUi.Col0 + Col) * mUi.CellWidth,
             mUi.PadY + (mUi.Row0 + Row) * mUi.CellHeight,
             Width,
             Height,
             0
             );
  FreePool (Buffer);
}

/**
  Draw the mockup screen: the logos, every text item, the boot-arg field, then the small art.
  The compact layout halves both logos and drops the rows that frees up.
**/
STATIC
VOID
UiDrawMain (
  VOID
  )
{
  UINTN   Index;
  UINTN   Row;
  UINTN   Shift;
  CHAR16  Field[B00T_UI_FIELD_WIDTH + 1];

  UiBlackout ();
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  //
  // Logos first: the star and the tagline are text printed over cells inside the logo.
  //
  if (mUi.Compact) {
    UiDrawCells (&mArtOtr[0][0], B00T_ART_OTR_COLS, B00T_ART_OTR_ROWS, B00T_ART_OTR_COL, 5, 6, 0xFF, 0xFF, 0xFF);
    UiDrawCells (&mArtBig[0][0], B00T_ART_BIG_COLS, B00T_ART_BIG_ROWS, B00T_ART_BIG_COL, 12, 4, 0x00, 0xFF, 0x00);
  } else {
    UiDrawCells (&mArtOtr[0][0], B00T_ART_OTR_COLS, B00T_ART_OTR_ROWS, B00T_ART_OTR_COL, B00T_ART_OTR_ROW, B00T_ART_OTR_ROWS, 0xFF, 0xFF, 0xFF);
    UiDrawCells (&mArtBig[0][0], B00T_ART_BIG_COLS, B00T_ART_BIG_ROWS, B00T_ART_BIG_COL, B00T_ART_BIG_ROW, B00T_ART_BIG_ROWS, 0x00, 0xFF, 0x00);
  }

  for (Index = 0; Index < ARRAY_SIZE (mUiItems); ++Index) {
    Row = mUi.Compact ? mUiItems[Index].CompactRow : mUiItems[Index].Row;
    if (Row == B00T_UI_HIDDEN) {
      continue;
    }

    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (mUiItems[Index].Color, EFI_BLACK));
    gST->ConOut->SetCursorPosition (gST->ConOut, mUi.Col0 + mUiItems[Index].Col, mUi.Row0 + Row);
    gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)mUiItems[Index].Text);
  }

  //
  // Rows below the logos move up by this much in the compact layout.
  //
  Shift = mUi.Shift;

  for (Index = 0; Index < B00T_UI_FIELD_WIDTH; ++Index) {
    Field[Index] = L' ';
  }

  Field[Index] = L'\0';
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY));
  gST->ConOut->SetCursorPosition (gST->ConOut, mUi.Col0 + B00T_UI_FIELD_COL, mUi.Row0 + B00T_UI_FIELD_ROW - Shift);
  gST->ConOut->OutputString (gST->ConOut, Field);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));

  UiDrawBits (mArtSkull, 8, 16, B00T_ART_SKULL_COL, B00T_ART_SKULL_ROW - Shift);
  UiDrawBits (mArtBanner, B00T_ART_BANNER_WIDTH, B00T_ART_BANNER_HEIGHT, B00T_ART_BANNER_COL, B00T_ART_BANNER_ROW - Shift);
}

//
// Phase 8c: the mockup screen wired up. Arrow keys move between the selectable
// items, Enter runs the selected one, Esc leaves. Sub-screens are plain text pages.
//
typedef enum {
  B00tUiStay,
  B00tUiExit,
  B00tUiReset
} B00T_UI_RESULT;

typedef struct {
  EFI_HANDLE                   ImageHandle;
  EFI_LOADED_IMAGE_PROTOCOL    *LoadedImage;
  EFI_FILE_PROTOCOL            *OcDir;
  EFI_FILE_PROTOCOL            *B00tDir;
  UINT8                        *ConfigData;
  UINTN                        ConfigSize;
  B00T_VARIANT                 *Variants;
  UINTN                        Count;
  CHAR16                       Serial[32];
  CHAR16                       ConfigBootArgs[B00T_MAX_ARGS];
} B00T_CONTEXT;

STATIC CONST CHAR16  *mNoYes[] = { L"NO  - go back", L"YES - do it" };

/**
  Copy at most DestinationSize - 1 characters of an ASCII string of at most SourceSize
  bytes into a UCS-2 buffer. Never asserts on long input.
**/
STATIC
VOID
AsciiToUnicodeN (
  IN  CONST CHAR8  *Source,
  IN  UINTN        SourceSize,
  OUT CHAR16       *Destination,
  IN  UINTN        DestinationSize
  )
{
  UINTN  Index;

  for (Index = 0; (Index < SourceSize) && (Index + 1 < DestinationSize) && (Source[Index] != '\0'); ++Index) {
    Destination[Index] = (CHAR16)(UINT8)Source[Index];
  }

  Destination[Index] = L'\0';
}

/**
  Print Text at a layout position, cut or padded with spaces to exactly Width cells.
**/
STATIC
VOID
UiPutText (
  IN UINTN         Row,
  IN UINTN         Col,
  IN UINTN         Width,
  IN UINTN         Attribute,
  IN CONST CHAR16  *Text
  )
{
  CHAR16  Buffer[B00T_UI_COLS + 1];
  UINTN   Index;

  Width = MIN (Width, B00T_UI_COLS);
  for (Index = 0; (Index < Width) && (Text[Index] != L'\0'); ++Index) {
    Buffer[Index] = Text[Index];
  }

  for ( ; Index < Width; ++Index) {
    Buffer[Index] = L' ';
  }

  Buffer[Width] = L'\0';
  gST->ConOut->SetAttribute (gST->ConOut, Attribute);
  gST->ConOut->SetCursorPosition (gST->ConOut, mUi.Col0 + Col, mUi.Row0 + Row);
  gST->ConOut->OutputString (gST->ConOut, Buffer);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
}

STATIC
VOID
UiStatus (
  IN UINTN         Attribute,
  IN CONST CHAR16  *Text
  )
{
  UiPutText (mUi.StatusRow, B00T_ART_BANNER_COL, B00T_UI_COLS - 2 * B00T_ART_BANNER_COL, Attribute, Text);
}

STATIC
VOID
UiDrawSel (
  IN UINTN    Index,
  IN BOOLEAN  Selected
  )
{
  UiPutText (
    mUiSel[Index].Row - mUi.Shift,
    mUiSel[Index].Col,
    StrLen (mUiSel[Index].Text),
    EFI_TEXT_ATTR (Selected ? EFI_YELLOW : EFI_LIGHTGRAY, EFI_BLACK),
    mUiSel[Index].Text
    );
}

/**
  Find the item an arrow key leads to: the nearest row in that direction, then the
  nearest centre; left and right stay on the row. Returns Current if there is none.
**/
STATIC
UINTN
UiNeighbour (
  IN UINTN   Current,
  IN UINT16  ScanCode
  )
{
  UINTN  Index;
  UINTN  Best;
  UINTN  BestScore;
  UINTN  Score;
  UINTN  Row;
  UINTN  Middle;
  UINTN  CurrentRow;
  UINTN  CurrentMiddle;

  Best          = Current;
  BestScore     = MAX_UINTN;
  CurrentRow    = mUiSel[Current].Row;
  CurrentMiddle = mUiSel[Current].Col * 2 + StrLen (mUiSel[Current].Text);

  for (Index = 0; Index < B00T_SEL_COUNT; ++Index) {
    Row    = mUiSel[Index].Row;
    Middle = mUiSel[Index].Col * 2 + StrLen (mUiSel[Index].Text);
    if (  (Index == Current)
       || ((ScanCode == SCAN_UP) && (Row >= CurrentRow))
       || ((ScanCode == SCAN_DOWN) && (Row <= CurrentRow))
       || ((ScanCode == SCAN_LEFT) && ((Row != CurrentRow) || (Middle >= CurrentMiddle)))
       || ((ScanCode == SCAN_RIGHT) && ((Row != CurrentRow) || (Middle <= CurrentMiddle))))
    {
      continue;
    }

    Score = (Row > CurrentRow ? Row - CurrentRow : CurrentRow - Row) * 1000
            + (Middle > CurrentMiddle ? Middle - CurrentMiddle : CurrentMiddle - Middle);
    if (Score < BestScore) {
      BestScore = Score;
      Best      = Index;
    }
  }

  return Best;
}

STATIC
VOID
UiPageBegin (
  IN CONST CHAR16  *Title
  )
{
  UiBlackout ();
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGREEN, EFI_BLACK));
  Print (L"B00tGeni3 - %s\n\n", Title);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
}

STATIC
VOID
UiPageEnd (
  VOID
  )
{
  Print (L"\nPress any key to go back...\n");
  WaitForKey ();
}

/**
  Start a fresh page when the current one is nearly full.
**/
STATIC
VOID
UiPageMore (
  IN CONST CHAR16  *Title
  )
{
  if ((UINTN)gST->ConOut->Mode->CursorRow + 6 >= mUi.Rows) {
    Print (L"\n-- more: press any key --");
    WaitForKey ();
    UiPageBegin (Title);
  }
}

STATIC
VOID
UiPrintWarning (
  IN CONST CHAR16  *Text
  )
{
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTRED, EFI_BLACK));
  //
  // Text is the format itself: Print only turns \n into \r\n in the format, not in a %s argument.
  // Callers pass literals without % signs.
  //
  Print (Text);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
}

/**
  Let the user pick one of Options, listed from the current cursor row down.

  @return  Index of the chosen option, or -1 on Esc.
**/
STATIC
INTN
UiChoose (
  IN CONST CHAR16  **Options,
  IN UINTN         Count,
  IN UINTN         Selected
  )
{
  EFI_INPUT_KEY  Key;
  UINTN          Top;
  UINTN          Index;

  Print (L"Up/Down to move, Enter to choose, Esc to cancel.\n\n");
  Top = (UINTN)gST->ConOut->Mode->CursorRow;
  while (TRUE) {
    for (Index = 0; Index < Count; ++Index) {
      gST->ConOut->SetCursorPosition (gST->ConOut, 0, Top + Index);
      gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (Index == Selected ? EFI_YELLOW : EFI_LIGHTGRAY, EFI_BLACK));
      Print (L"  %s %s\n", Index == Selected ? L">" : L" ", Options[Index]);
    }

    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
    Key = WaitForKey ();
    if ((Key.ScanCode == SCAN_UP) && (Selected > 0)) {
      --Selected;
    } else if ((Key.ScanCode == SCAN_DOWN) && (Selected + 1 < Count)) {
      ++Selected;
    } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      return (INTN)Selected;
    } else if (Key.ScanCode == SCAN_ESC) {
      return -1;
    }
  }
}

/**
  Read the serial number and the boot-args string out of the active config.plist.
**/
STATIC
VOID
LoadConfigInfo (
  IN OUT B00T_CONTEXT  *Context
  )
{
  EFI_STATUS        Status;
  OC_GLOBAL_CONFIG  *Config;
  UINT8             *Copy;
  UINT32            ErrorCount;
  UINT32            Index;
  UINT32            Index2;
  OC_ASSOC          *Map;

  Context->Serial[0]         = L'\0';
  Context->ConfigBootArgs[0] = L'\0';

  Copy = AllocateCopyPool (Context->ConfigSize, Context->ConfigData);
  if (Copy == NULL) {
    return;
  }

  Config     = &mPreflightConfig;
  ErrorCount = 0;
  Status     = OcConfigurationInit (Config, Copy, (UINT32)Context->ConfigSize, &ErrorCount);
  if (EFI_ERROR (Status)) {
    FreePool (Copy);
    return;
  }

  AsciiToUnicodeN (
    OC_BLOB_GET (&Config->PlatformInfo.Generic.SystemSerialNumber),
    Config->PlatformInfo.Generic.SystemSerialNumber.Size,
    Context->Serial,
    ARRAY_SIZE (Context->Serial)
    );

  for (Index = 0; Index < Config->Nvram.Add.Count; ++Index) {
    if (AsciiStriCmp (OC_BLOB_GET (Config->Nvram.Add.Keys[Index]), B00T_APPLE_BOOT_GUID) != 0) {
      continue;
    }

    Map = Config->Nvram.Add.Values[Index];
    for (Index2 = 0; Index2 < Map->Count; ++Index2) {
      if (AsciiStrCmp (OC_BLOB_GET (Map->Keys[Index2]), "boot-args") == 0) {
        AsciiToUnicodeN (
          (CONST CHAR8 *)OC_BLOB_GET (Map->Values[Index2]),
          Map->Values[Index2]->Size,
          Context->ConfigBootArgs,
          ARRAY_SIZE (Context->ConfigBootArgs)
          );
      }
    }
  }

  OcConfigurationFree (Config);
  FreePool (Copy);
}

/**
  Short name for the partition a handle sits on, such as "SATA p1 GPT".
**/
STATIC
VOID
BootDeviceText (
  IN  EFI_HANDLE  Handle,
  OUT CHAR16      *Text,
  IN  UINTN       TextSize
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  HARDDRIVE_DEVICE_PATH     *HardDrive;
  CONST CHAR16              *Bus;

  Bus       = L"disk";
  HardDrive = NULL;
  for (Node = DevicePathFromHandle (Handle); (Node != NULL) && !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if (DevicePathType (Node) == MESSAGING_DEVICE_PATH) {
      switch (DevicePathSubType (Node)) {
        case MSG_SATA_DP:
          Bus = L"SATA";
          break;
        case MSG_NVME_NAMESPACE_DP:
          Bus = L"NVMe";
          break;
        case MSG_USB_DP:
          Bus = L"USB";
          break;
        case MSG_ATAPI_DP:
          Bus = L"ATA";
          break;
        case MSG_SCSI_DP:
          Bus = L"SCSI";
          break;
        case MSG_SD_DP:
          Bus = L"SD";
          break;
        case MSG_EMMC_DP:
          Bus = L"eMMC";
          break;
        default:
          break;
      }
    } else if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) && (DevicePathSubType (Node) == MEDIA_HARDDRIVE_DP)) {
      HardDrive = (HARDDRIVE_DEVICE_PATH *)Node;
    }
  }

  if (HardDrive != NULL) {
    UnicodeSPrint (
      Text,
      TextSize,
      L"%s p%u %s",
      Bus,
      ReadUnaligned32 (&HardDrive->PartitionNumber),
      HardDrive->SignatureType == SIGNATURE_TYPE_GUID ? L"GPT" : L"MBR"
      );
  } else {
    UnicodeSPrint (Text, TextSize, L"%s", Bus);
  }
}

/**
  Fill in the system info row and the boot-arg field. Called on every redraw, so a
  SIP toggle shows up when its page closes.
**/
STATIC
VOID
UiDrawInfo (
  IN CONST B00T_CONTEXT  *Context
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   *Handles;
  UINTN                        HandleCount;
  EFI_SIMPLE_NETWORK_PROTOCOL  *Network;
  CHAR16                       Text[B00T_MAX_ARGS];
  CHAR8                        Args[B00T_MAX_ARGS];
  UINTN                        Size;
  UINT32                       Csr;
  UINTN                        Value;

  Value = EFI_TEXT_ATTR (EFI_LIGHTGREEN, EFI_BLACK);

  BootDeviceText (Context->LoadedImage->DeviceHandle, Text, sizeof (Text));
  UiPutText (0, 47, 16, Value, Text);

  StrCpyS (Text, ARRAY_SIZE (Text), L"n/a");
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleNetworkProtocolGuid, NULL, &HandleCount, &Handles);
  if (!EFI_ERROR (Status)) {
    if (  (HandleCount > 0)
       && !EFI_ERROR (gBS->HandleProtocol (Handles[0], &gEfiSimpleNetworkProtocolGuid, (VOID **)&Network))
       && (Network->Mode != NULL))
    {
      UnicodeSPrint (
        Text,
        sizeof (Text),
        L"%02X%02X%02X%02X%02X%02X",
        Network->Mode->CurrentAddress.Addr[0],
        Network->Mode->CurrentAddress.Addr[1],
        Network->Mode->CurrentAddress.Addr[2],
        Network->Mode->CurrentAddress.Addr[3],
        Network->Mode->CurrentAddress.Addr[4],
        Network->Mode->CurrentAddress.Addr[5]
        );
    }

    FreePool (Handles);
  }

  UiPutText (0, 69, 15, Value, Text);
  UiPutText (0, 93, 29, Value, Context->Serial[0] != L'\0' ? Context->Serial : L"n/a");

  Size   = sizeof (Csr);
  Status = gRT->GetVariable (L"csr-active-config", &gAppleBootVariableGuid, NULL, &Size, &Csr);
  if (!EFI_ERROR (Status) && (Size == sizeof (Csr))) {
    UnicodeSPrint (Text, sizeof (Text), L"0x%08X", Csr);
  } else {
    StrCpyS (Text, ARRAY_SIZE (Text), L"not set");
  }

  UiPutText (1, 47, 10, Value, Text);

  Size   = sizeof (Args) - 1;
  Status = gRT->GetVariable (L"boot-args", &gAppleBootVariableGuid, NULL, &Size, Args);
  if (!EFI_ERROR (Status)) {
    AsciiToUnicodeN (Args, Size, Text, ARRAY_SIZE (Text));
  } else {
    StrCpyS (Text, ARRAY_SIZE (Text), L"not set");
  }

  UiPutText (1, 69, 53, Value, Text);

  UiPutText (
    B00T_UI_FIELD_ROW - mUi.Shift,
    B00T_UI_FIELD_COL,
    B00T_UI_FIELD_WIDTH,
    EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY),
    Context->ConfigBootArgs
    );
}

/**
  Pre-flight a variant, ask, and swap it in through the Phase 4 path.
**/
STATIC
B00T_UI_RESULT
UiActivate (
  IN B00T_CONTEXT        *Context,
  IN CONST B00T_VARIANT  *Variant  OPTIONAL
  )
{
  EFI_STATUS  Status;

  UiPageBegin (L"switch config");
  if (Variant == NULL) {
    UiPrintWarning (L"No " B00T_KNOWN_GOOD L" in " B00T_DIR_PATH L".\n");
    UiPageEnd ();
    return B00tUiStay;
  }

  if (Variant->Active) {
    Print (L"%s is already active.\n", Variant->Name);
    UiPageEnd ();
    return B00tUiStay;
  }

  Print (L"Pre-flight check of %s:\n", Variant->Name);
  if (!PreflightVariant (Context->OcDir, Variant)) {
    UiPrintWarning (L"\nREFUSED: not safe to activate. Nothing was changed.\n");
    UiPageEnd ();
    return B00tUiStay;
  }

  Print (L"\nSwitch config.plist to %s and cold reset?\n", Variant->Name);
  if (UiChoose (mNoYes, ARRAY_SIZE (mNoYes), 0) != 1) {
    return B00tUiStay;
  }

  Print (L"\n");
  Status = SwapConfig (Context->OcDir, Context->B00tDir, Context->ConfigData, Context->ConfigSize, Variant);
  if (EFI_ERROR (Status)) {
    UiPrintWarning (L"\nSWITCH FAILED. No reset.\n");
    Print (L"Status: %r\n", Status);
    UiPageEnd ();
    return B00tUiExit;
  }

  Print (L"\nSwitched to %s. Cold reset...\n", Variant->Name);
  DEBUG ((DEBUG_INFO, "B00T: Switched to %s, cold reset\n", Variant->Name));
  return B00tUiReset;
}

STATIC
B00T_UI_RESULT
UiLoadAlternate (
  IN B00T_CONTEXT  *Context
  )
{
  CONST CHAR16  *Names[B00T_MAX_VARIANTS];
  CHAR16        Lines[B00T_MAX_VARIANTS][B00T_MAX_NAME + 16];
  UINTN         Index;
  INTN          Choice;

  UiPageBegin (L"LOAD-ALTERNATE");
  Print (L"Config variants in %s:\n\n", B00T_DIR_PATH);
  for (Index = 0; Index < Context->Count; ++Index) {
    UnicodeSPrint (
      Lines[Index],
      sizeof (Lines[Index]),
      L"%-28s%s",
      Context->Variants[Index].Name,
      Context->Variants[Index].Active ? L"<- ACTIVE" : L""
      );
    Names[Index] = Lines[Index];
  }

  Choice = UiChoose (Names, Context->Count, 0);
  if (Choice < 0) {
    return B00tUiStay;
  }

  return UiActivate (Context, &Context->Variants[Choice]);
}

/**
  Load and start a tool from EFI\OC\Tools on the volume this tool runs from.
**/
STATIC
EFI_STATUS
UiRunTool (
  IN B00T_CONTEXT  *Context,
  IN CONST CHAR16  *Name,
  IN CHAR16        *Options  OPTIONAL
  )
{
  EFI_STATUS                 Status;
  EFI_DEVICE_PATH_PROTOCOL   *DevicePath;
  EFI_LOADED_IMAGE_PROTOCOL  *Child;
  EFI_HANDLE                 Handle;
  CHAR16                     Path[B00T_MAX_PATH];

  UnicodeSPrint (Path, sizeof (Path), L"\\%s\\Tools\\%s", B00T_OC_DIR_PATH, Name);
  DevicePath = FileDevicePath (Context->LoadedImage->DeviceHandle, Path);
  if (DevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Handle = NULL;
  Status = gBS->LoadImage (FALSE, Context->ImageHandle, DevicePath, NULL, 0, &Handle);
  FreePool (DevicePath);
  if (EFI_ERROR (Status)) {
    if ((Status == EFI_SECURITY_VIOLATION) && (Handle != NULL)) {
      gBS->UnloadImage (Handle);
    }

    UiPrintWarning (L"Cannot load the tool.\n");
    Print (L"%s - %r\n", Path, Status);
    return Status;
  }

  if (Options != NULL) {
    Status = gBS->HandleProtocol (Handle, &gEfiLoadedImageProtocolGuid, (VOID **)&Child);
    if (EFI_ERROR (Status)) {
      gBS->UnloadImage (Handle);
      return Status;
    }

    Child->LoadOptions     = Options;
    Child->LoadOptionsSize = (UINT32)StrSize (Options);
  }

  return gBS->StartImage (Handle, NULL, NULL);
}

STATIC
B00T_UI_RESULT
UiNvramReset (
  IN B00T_CONTEXT  *Context
  )
{
  EFI_STATUS  Status;

  UiPageBegin (L"NVRAM-RESET");
  UiPrintWarning (L"WARNING: CleanNvram.efi deletes NVRAM variables and does not preserve firmware boot entries.\n");
  Print (L"OpenCore sets the variables in its config again on the next boot.\n\n");
  Print (L"Run CleanNvram.efi, then cold reset?\n");
  if (UiChoose (mNoYes, ARRAY_SIZE (mNoYes), 0) != 1) {
    return B00tUiStay;
  }

  Print (L"\n");
  Status = UiRunTool (Context, L"CleanNvram.efi", NULL);
  Print (L"\nCleanNvram.efi returned %r.\n", Status);
  if (EFI_ERROR (Status)) {
    UiPageEnd ();
    return B00tUiStay;
  }

  Print (L"Cold reset...\n");
  return B00tUiReset;
}

STATIC
B00T_UI_RESULT
UiToggleSip (
  IN B00T_CONTEXT  *Context
  )
{
  EFI_STATUS  Status;
  UINTN       Size;
  UINT32      Csr;

  UiPageBegin (L"DISABLE SIP - ENABLE SIP");
  Size   = sizeof (Csr);
  Status = gRT->GetVariable (L"csr-active-config", &gAppleBootVariableGuid, NULL, &Size, &Csr);
  if (!EFI_ERROR (Status) && (Size == sizeof (Csr))) {
    Print (L"csr-active-config is 0x%08X now.\n\n", Csr);
  } else {
    Print (L"csr-active-config is not set now (%r).\n\n", Status);
  }

  Print (L"Toggle SIP with CsrUtil.efi?\n");
  if (UiChoose (mNoYes, ARRAY_SIZE (mNoYes), 0) != 1) {
    return B00tUiStay;
  }

  Print (L"\n");
  Status = UiRunTool (Context, L"CsrUtil.efi", L"toggle");
  Print (L"\nCsrUtil.efi returned %r.\n", Status);
  UiPageEnd ();
  return B00tUiStay;
}

STATIC
B00T_UI_RESULT
UiShell (
  IN B00T_CONTEXT  *Context
  )
{
  EFI_STATUS  Status;

  UiPageBegin (L"UEFI SHELL (type exit to come back)");
  gST->ConOut->EnableCursor (gST->ConOut, TRUE);
  Status = UiRunTool (Context, L"OpenShell.efi", NULL);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  if (EFI_ERROR (Status)) {
    Print (L"\nOpenShell.efi returned %r.\n", Status);
    UiPageEnd ();
  }

  return B00tUiStay;
}

STATIC
B00T_UI_RESULT
UiDumpDiskMap (
  IN B00T_CONTEXT  *Context
  )
{
  STATIC CONST CHAR16    *Title = L"DUMP-DISKMAP";
  EFI_STATUS             Status;
  EFI_HANDLE             *Handles;
  UINTN                  HandleCount;
  UINTN                  Index;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  VOID                   *FileSystem;
  CHAR16                 *PathText;
  UINT64                 Megabytes;

  UiPageBegin (Title);
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    Print (L"No block devices - %r\n", Status);
    UiPageEnd ();
    return B00tUiStay;
  }

  Print (L"%u block devices. FS = has a file system; this tool runs from the one marked.\n\n", (UINT32)HandleCount);
  for (Index = 0; Index < HandleCount; ++Index) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || (BlockIo->Media == NULL)) {
      continue;
    }

    UiPageMore (Title);
    Megabytes = BlockIo->Media->MediaPresent
                  ? RShiftU64 (MultU64x32 (BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize), 20)
                  : 0;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, &FileSystem);
    Print (
      L"%2u. %s %8Lu MB  %s  %s%s\n",
      (UINT32)(Index + 1),
      BlockIo->Media->LogicalPartition ? L"partition" : L"disk     ",
      Megabytes,
      BlockIo->Media->RemovableMedia ? L"removable" : L"fixed    ",
      !EFI_ERROR (Status) ? L"FS" : L"  ",
      Handles[Index] == Context->LoadedImage->DeviceHandle ? L"  <- B00tGeni3 runs from here" : L""
      );
    PathText = ConvertDevicePathToText (DevicePathFromHandle (Handles[Index]), FALSE, FALSE);
    Print (L"    %s\n", PathText != NULL ? PathText : L"<no device path>");
    if (PathText != NULL) {
      FreePool (PathText);
    }
  }

  FreePool (Handles);
  UiPageEnd ();
  return B00tUiStay;
}

/**
  Write the ACPI tables to EFI\OC\B00tGeni3\ACPI-<date>-<time> with OcAcpiLib's
  AcpiDumpTables, the function SysReport uses, and list what was written.
**/
STATIC
B00T_UI_RESULT
UiDumpAcpi (
  IN B00T_CONTEXT  *Context
  )
{
  STATIC CONST CHAR16  *Title = L"DUMP SSDT";
  EFI_STATUS           Status;
  EFI_TIME             Time;
  EFI_FILE_PROTOCOL    *Folder;
  EFI_FILE_INFO        *Info;
  CHAR16               Name[32];
  UINTN                Size;
  UINTN                Files;
  UINT64               Total;

  UiPageBegin (Title);
  Status = gRT->GetTime (&Time, NULL);
  if (EFI_ERROR (Status)) {
    ZeroMem (&Time, sizeof (Time));
  }

  UnicodeSPrint (
    Name,
    sizeof (Name),
    L"ACPI-%04u%02u%02u-%02u%02u%02u",
    Time.Year,
    Time.Month,
    Time.Day,
    Time.Hour,
    Time.Minute,
    Time.Second
    );
  Status = Context->B00tDir->Open (
                               Context->B00tDir,
                               &Folder,
                               Name,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                               EFI_FILE_DIRECTORY
                               );
  if (EFI_ERROR (Status)) {
    UiPrintWarning (L"Cannot create the folder.\n");
    Print (L"%s\\%s - %r\n", B00T_DIR_PATH, Name, Status);
    UiPageEnd ();
    return B00tUiStay;
  }

  Status = AcpiDumpTables (Folder);
  Print (L"ACPI tables -> %s\\%s - %r\n\n", B00T_DIR_PATH, Name, Status);

  Files = 0;
  Total = 0;
  Info  = AllocatePool (SIZE_1KB);
  if (Info != NULL) {
    Folder->SetPosition (Folder, 0);
    while (TRUE) {
      Size   = SIZE_1KB;
      Status = Folder->Read (Folder, &Size, Info);
      if (EFI_ERROR (Status) || (Size == 0)) {
        break;
      }

      if ((Info->Attribute & EFI_FILE_DIRECTORY) != 0) {
        continue;
      }

      UiPageMore (Title);
      Print (L"  %-16s %8Lu bytes\n", Info->FileName, Info->FileSize);
      ++Files;
      Total += Info->FileSize;
    }

    FreePool (Info);
  }

  Print (L"\n%u files, %Lu bytes.\n", (UINT32)Files, Total);
  Folder->Flush (Folder);
  Folder->Close (Folder);
  UiPageEnd ();
  return B00tUiStay;
}

/**
  Run the mockup screen until the user leaves or an action needs a cold reset.
**/
STATIC
B00T_UI_RESULT
UiRun (
  IN OUT B00T_CONTEXT  *Context
  )
{
  EFI_INPUT_KEY   Key;
  B00T_UI_RESULT  Result;
  UINTN           Selected;
  UINTN           Next;
  UINTN           Index;
  BOOLEAN         Redraw;
  B00T_VARIANT    *KnownGood;

  LoadConfigInfo (Context);
  KnownGood = NULL;
  for (Index = 0; Index < Context->Count; ++Index) {
    if (B00tStriEqual (Context->Variants[Index].Name, B00T_KNOWN_GOOD)) {
      KnownGood = &Context->Variants[Index];
    }
  }

  Selected = B00T_SEL_LOAD;
  Redraw   = TRUE;
  while (TRUE) {
    if (Redraw) {
      UiDrawMain ();
      UiDrawInfo (Context);
      for (Index = 0; Index < B00T_SEL_COUNT; ++Index) {
        UiDrawSel (Index, Index == Selected);
      }

      UiStatus (EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK), mUiSel[Selected].Hint);
      Redraw = FALSE;
    }

    Key = WaitForKey ();
    if (Key.ScanCode == SCAN_ESC) {
      return B00tUiExit;
    }

    if ((Key.ScanCode == SCAN_UP) || (Key.ScanCode == SCAN_DOWN) || (Key.ScanCode == SCAN_LEFT) || (Key.ScanCode == SCAN_RIGHT)) {
      Next = UiNeighbour (Selected, Key.ScanCode);
      if (Next != Selected) {
        UiDrawSel (Selected, FALSE);
        Selected = Next;
        UiDrawSel (Selected, TRUE);
        UiStatus (EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK), mUiSel[Selected].Hint);
      }

      continue;
    }

    if (Key.UnicodeChar != CHAR_CARRIAGE_RETURN) {
      continue;
    }

    switch (Selected) {
      case B00T_SEL_DISCARD:
        return B00tUiExit;
      case B00T_SEL_RESTORE:
        Result = UiActivate (Context, KnownGood);
        break;
      case B00T_SEL_LOAD:
        Result = UiLoadAlternate (Context);
        break;
      case B00T_SEL_NVRAM:
        Result = UiNvramReset (Context);
        break;
      case B00T_SEL_SIP:
        Result = UiToggleSip (Context);
        break;
      case B00T_SEL_SHELL:
        Result = UiShell (Context);
        break;
      case B00T_SEL_DISKMAP:
        Result = UiDumpDiskMap (Context);
        break;
      case B00T_SEL_SSDT:
        Result = UiDumpAcpi (Context);
        break;
      default:
        //
        // Not available yet: say so in the warning color.
        //
        UiStatus (EFI_TEXT_ATTR (EFI_LIGHTRED, EFI_BLACK), mUiSel[Selected].Hint);
        continue;
    }

    if (Result != B00tUiStay) {
      return Result;
    }

    Redraw = TRUE;
  }
}

STATIC
VOID
ShowMenu (
  IN CONST UINT8         *ConfigData,
  IN UINTN               ConfigSize,
  IN CONST CHAR8         *ActiveName,
  IN CONST B00T_VARIANT  *Variants,
  IN UINTN               Count
  )
{
  UINT8    Hash[SHA256_DIGEST_SIZE];
  UINTN    Index;
  BOOLEAN  AnyActive;

  Print (L"\nconfig.plist: %u bytes, sha256 ", (UINT32)ConfigSize);
  Sha256 (Hash, ConfigData, ConfigSize);
  PrintHashPrefix (Hash);
  Print (L"...\nRecorded active: %a\n\n", ActiveName != NULL ? ActiveName : "<none>");

  AnyActive = FALSE;
  for (Index = 0; Index < Count; ++Index) {
    Print (L"  %u. %-24s %6u bytes  ", (UINT32)(Index + 1), Variants[Index].Name, (UINT32)Variants[Index].Size);
    PrintHashPrefix (Variants[Index].Hash);
    Print (L"...%s\n", Variants[Index].Active ? L"  <- ACTIVE" : L"");
    AnyActive |= Variants[Index].Active;
  }

  if (!AnyActive) {
    Print (L"\n  config.plist matches none of these variants.\n");
  }

  Print (L"\nPress 1-%u to switch, Esc to return to OpenCore.\n", (UINT32)Count);
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *OcDir;
  EFI_FILE_PROTOCOL                *B00tDir;
  UINT8                            *ConfigData;
  UINTN                            ConfigSize;
  UINT8                            *ActiveData;
  UINTN                            ActiveSize;
  CHAR8                            ActiveName[B00T_MAX_NAME];
  B00T_VARIANT                     Variants[B00T_MAX_VARIANTS];
  UINTN                            Count;
  EFI_INPUT_KEY                    Key;
  UINTN                            Choice;
  BOOLEAN                          Swapped;
  STATIC B00T_CONTEXT              UiContext;

  Root    = NULL;
  OcDir   = NULL;
  B00tDir = NULL;
  Count   = 0;
  Swapped = FALSE;

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"B00tGeni3 %s - config picker\n\n", B00T_VERSION);

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  if (!EFI_ERROR (Status)) {
    PrintDevicePath (L"Running from", LoadedImage->FilePath);
    //
    // OpenProtocol, not HandleProtocol: OpenCore hooks HandleProtocol (OcVirtualFsLib) and
    // returns a wrapped file system whose Delete only closes the file.
    //
    Status = gBS->OpenProtocol (
                    LoadedImage->DeviceHandle,
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&FileSystem,
                    ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
  }

  if (!EFI_ERROR (Status)) {
    Status = FileSystem->OpenVolume (FileSystem, &Root);
  }

  if (!EFI_ERROR (Status)) {
    Status = Root->Open (Root, &OcDir, B00T_OC_DIR_PATH, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Root->Open (Root, &B00tDir, B00T_DIR_PATH, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  }

  if (EFI_ERROR (Status)) {
    Print (L"Cannot open %s on the boot volume - %r\n", B00T_DIR_PATH, Status);
    goto Done;
  }

  Status = ReadWholeFile (OcDir, B00T_CONFIG_NAME, &ConfigData, &ConfigSize);
  if (EFI_ERROR (Status)) {
    Print (L"Cannot read %s\\%s - %r\n", B00T_OC_DIR_PATH, B00T_CONFIG_NAME, Status);
    goto Done;
  }

  ZeroMem (ActiveName, sizeof (ActiveName));
  if (!EFI_ERROR (ReadWholeFile (B00tDir, B00T_ACTIVE_NAME, &ActiveData, &ActiveSize))) {
    CopyMem (ActiveName, ActiveData, MIN (ActiveSize, sizeof (ActiveName) - 1));
    for (Choice = 0; ActiveName[Choice] != '\0'; ++Choice) {
      if ((ActiveName[Choice] == '\r') || (ActiveName[Choice] == '\n')) {
        ActiveName[Choice] = '\0';
        break;
      }
    }

    FreePool (ActiveData);
  }

  Status = LoadVariants (B00tDir, ConfigData, ConfigSize, Variants, &Count);
  if (EFI_ERROR (Status) || (Count == 0)) {
    Print (L"No variants found in %s - %r\n", B00T_DIR_PATH, Status);
    FreePool (ConfigData);
    goto Done;
  }

  //
  // The mockup screen when the console can show it, otherwise the plain menu below.
  //
  if (UiInit ()) {
    UiContext.ImageHandle = ImageHandle;
    UiContext.LoadedImage = LoadedImage;
    UiContext.OcDir       = OcDir;
    UiContext.B00tDir     = B00tDir;
    UiContext.ConfigData  = ConfigData;
    UiContext.ConfigSize  = ConfigSize;
    UiContext.Variants    = Variants;
    UiContext.Count       = Count;
    if (UiRun (&UiContext) == B00tUiReset) {
      Swapped = TRUE;
    } else {
      UiBlackout ();
    }

    gST->ConOut->EnableCursor (gST->ConOut, TRUE);
    goto Finish;
  }

  while (TRUE) {
    ShowMenu (ConfigData, ConfigSize, ActiveName[0] != '\0' ? ActiveName : NULL, Variants, Count);
    Key = WaitForKey ();
    if ((Key.ScanCode == SCAN_ESC) || (Key.UnicodeChar == L'q') || (Key.UnicodeChar == L'Q')) {
      break;
    }

    if ((Key.UnicodeChar == L'g') || (Key.UnicodeChar == L'G')) {
      GlyphTest ();
      WaitForKey ();
      gST->ConOut->ClearScreen (gST->ConOut);
      continue;
    }

    if ((Key.UnicodeChar < L'1') || (Key.UnicodeChar > L'0' + Count)) {
      continue;
    }

    Choice = Key.UnicodeChar - L'1';
    if (Variants[Choice].Active) {
      Print (L"\n%s is already active.\n", Variants[Choice].Name);
      continue;
    }

    Print (L"\nPre-flight check of %s:\n", Variants[Choice].Name);
    if (!PreflightVariant (OcDir, &Variants[Choice])) {
      Print (L"\nREFUSED: %s is not safe to activate. Nothing was changed.\n", Variants[Choice].Name);
      Print (L"Press any key for the menu...\n");
      WaitForKey ();
      gST->ConOut->ClearScreen (gST->ConOut);
      continue;
    }

    Print (L"\nSwitch config.plist to %s? Y = yes, any other key = cancel\n", Variants[Choice].Name);
    Key = WaitForKey ();
    if ((Key.UnicodeChar != L'y') && (Key.UnicodeChar != L'Y')) {
      continue;
    }

    Print (L"\n");
    Status = SwapConfig (OcDir, B00tDir, ConfigData, ConfigSize, &Variants[Choice]);
    if (EFI_ERROR (Status)) {
      Print (L"\nSWITCH FAILED - %r. No reset.\n", Status);
    } else {
      Print (L"\nSwitched to %s. Cold reset...\n", Variants[Choice].Name);
      DEBUG ((DEBUG_INFO, "B00T: Switched to %s, cold reset\n", Variants[Choice].Name));
      Swapped = TRUE;
    }

    break;
  }

Finish:
  FreeVariants (Variants, Count);
  FreePool (ConfigData);

Done:
  if (B00tDir != NULL) {
    B00tDir->Flush (B00tDir);
    B00tDir->Close (B00tDir);
  }

  if (OcDir != NULL) {
    OcDir->Flush (OcDir);
    OcDir->Close (OcDir);
  }

  if (Root != NULL) {
    Root->Flush (Root);
    Root->Close (Root);
  }

  if (Swapped) {
    gBS->Stall (2000000);
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  }

  if (EFI_ERROR (Status)) {
    Print (L"\nPress any key to return to OpenCore...\n");
    WaitForKey ();
  }

  return Status;
}
