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
  ShowPicker is true, this tool is still an enabled Tools entry, every file
  referenced by an enabled Drivers, Kexts, ACPI or Tools entry exists, and the
  vault check passes. A config failing these could stop OpenCore before the
  picker, where this tool cannot be reached to switch back.

  Vault (Phase 7): OpenCore checks every file it reads against OC\vault.plist
  whenever that file exists, and needs OC\vault.sig to match the key patched
  into OpenCore.efi when there is one. A variant that needs a vault brings its
  own set, signed on the Mac (the tool never signs): NAME.vault and NAME.vsig
  next to NAME.plist. The pre-flight checks the set against the variant and
  against every file under EFI\OC, and the swap writes all three files.

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
#include <Library/OcSerializeLib.h>
#include <Library/OcStorageLib.h>
#include <Library/OcStringLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "B00tArt.h"

#define B00T_VERSION         L"0.8g"
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
#define B00T_VAULT_NAME      L"vault.plist"
#define B00T_VAULT_TMP       L"vault.tmp"
#define B00T_VSIG_NAME       L"vault.sig"
#define B00T_VSIG_TMP        L"vault.sig.tmp"
#define B00T_VAULT_BAK       L"vault.plist.bak"
#define B00T_VAULT_BAK_TMP   L"vault.bak.tmp"
#define B00T_VSIG_BAK        L"vault.sig.bak"
#define B00T_VSIG_BAK_TMP    L"vsig.bak.tmp"
#define B00T_SET_VAULT_EXT   L".vault"
#define B00T_SET_SIG_EXT     L".vsig"
#define B00T_OC_EFI_NAME     L"OpenCore.efi"
#define B00T_MAX_EFI_SIZE    SIZE_8MB
#define B00T_MAX_DISK_FILES  1024
#define B00T_VAULT_SIG_SIZE  256
#define B00T_VAULT_KEY_SIZE  528
#define B00T_MAX_DETAIL      6

typedef struct {
  CHAR16     Name[B00T_MAX_NAME];
  UINT8      *Data;
  UINTN      Size;
  UINT8      Hash[SHA256_DIGEST_SIZE];
  BOOLEAN    Active;
  EFI_STATUS ReadStatus;  // not EFI_SUCCESS: the file could not be read, Data is a 1-byte empty buffer
  UINT8      *VaultData;  // NAME.vault from the variant folder, NULL if there is none
  UINTN      VaultSize;
  UINT8      *SigData;    // NAME.vsig, NULL if there is none
  UINTN      SigSize;
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
ReadWholeFileMax (
  IN  EFI_FILE_PROTOCOL  *Dir,
  IN  CONST CHAR16       *Name,
  IN  UINT64             MaxSize,
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
  if (EFI_ERROR (Status) || (Info->FileSize > MaxSize)) {
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
ReadWholeFile (
  IN  EFI_FILE_PROTOCOL  *Dir,
  IN  CONST CHAR16       *Name,
  OUT UINT8              **Data,
  OUT UINTN              *Size
  )
{
  return ReadWholeFileMax (Dir, Name, B00T_MAX_FILE_SIZE, Data, Size);
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

    if (Variants[Index].VaultData != NULL) {
      FreePool (Variants[Index].VaultData);
      Variants[Index].VaultData = NULL;
    }

    if (Variants[Index].SigData != NULL) {
      FreePool (Variants[Index].SigData);
      Variants[Index].SigData = NULL;
    }
  }
}

/**
  Read the variant's vault set (NAME.vault, NAME.vsig) if it has one. A missing file
  leaves the pointer NULL; the pre-flight decides whether that matters.
**/
STATIC
VOID
LoadVaultSet (
  IN     EFI_FILE_PROTOCOL  *B00tDir,
  IN OUT B00T_VARIANT       *Variant
  )
{
  EFI_STATUS  Status;
  CHAR16      SetName[B00T_MAX_NAME + 8];
  UINTN       StemLength;

  StemLength = StrLen (Variant->Name) - 6;  // without ".plist"
  CopyMem (SetName, Variant->Name, StemLength * sizeof (CHAR16));
  SetName[StemLength] = L'\0';
  StrCatS (SetName, ARRAY_SIZE (SetName), B00T_SET_VAULT_EXT);
  Status = ReadWholeFile (B00tDir, SetName, &Variant->VaultData, &Variant->VaultSize);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    Print (L"Cannot read %s - %r\n", SetName, Status);
  }

  SetName[StemLength] = L'\0';
  StrCatS (SetName, ARRAY_SIZE (SetName), B00T_SET_SIG_EXT);
  Status = ReadWholeFile (B00tDir, SetName, &Variant->SigData, &Variant->SigSize);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    Print (L"Cannot read %s - %r\n", SetName, Status);
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
      //
      // Keep it in the list as an empty file so the pre-flight refuses it with this status.
      // One bad file must not hide the others.
      //
      Print (L"Cannot read %s - %r\n", Info->FileName, Status);
      Variants[*Count].ReadStatus = Status;
      Variants[*Count].Size       = 0;
      Variants[*Count].Data       = AllocateZeroPool (1);
      if (Variants[*Count].Data == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        break;
      }

      Status = EFI_SUCCESS;
    } else {
      LoadVaultSet (B00tDir, &Variants[*Count]);
    }

    Sha256 (Variants[*Count].Hash, Variants[*Count].Data, Variants[*Count].Size);
    Variants[*Count].Active = (ConfigData != NULL)
                              && (Variants[*Count].Size == ConfigSize)
                              && (CompareMem (Variants[*Count].Data, ConfigData, ConfigSize) == 0);
    ++(*Count);
  }

  FreePool (Info);
  if (EFI_ERROR (Status) && (*Count > 0) && (Status != EFI_OUT_OF_RESOURCES)) {
    //
    // The directory listing failed part way: keep what loaded, known-good may be among it.
    //
    Print (L"Reading %s stopped early - %r\n", B00T_DIR_PATH, Status);
    Status = EFI_SUCCESS;
  }

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
// Vault. The schema is OpenCore's own (OcStorageLib.c), which does not export it.
//
STATIC
OC_SCHEMA
  mB00tVaultFilesSchema = OC_SCHEMA_DATAF (NULL, UINT8[SHA256_DIGEST_SIZE]);

///
/// WARNING: Field list must be alpabetically ordered here!
///
STATIC
OC_SCHEMA
  mB00tVaultNodesSchema[] = {
  OC_SCHEMA_MAP_IN ("Files",       OC_STORAGE_VAULT, Files,    &mB00tVaultFilesSchema),
  OC_SCHEMA_INTEGER_IN ("Version", OC_STORAGE_VAULT, Version),
};

STATIC
OC_SCHEMA_INFO
  mB00tVaultSchema = {
  .Dict = { mB00tVaultNodesSchema, ARRAY_SIZE (mB00tVaultNodesSchema) }
};

typedef struct {
  CHAR8      Path[OC_STORAGE_SAFE_PATH_MAX];  // as vault.plist names it: Drivers\OpenRuntime.efi
  UINT8      Hash[SHA256_DIGEST_SIZE];
  EFI_STATUS Status;                          // not EFI_SUCCESS: could not be hashed
} B00T_DISK_FILE;

//
// What is on the disk now. Read once per run: nothing the tool writes before a reset is
// covered by a vault (config.plist is compared separately, the B00tGeni3 folder is skipped).
//
STATIC BOOLEAN            mVaultStateRead;
STATIC OC_RSA_PUBLIC_KEY  *mVaultKey;       // key patched into OC\OpenCore.efi, NULL if none
STATIC EFI_STATUS         mVaultKeyStatus;  // reading OpenCore.efi
STATIC BOOLEAN            mVaultPlistOnDisk;
STATIC BOOLEAN            mDiskScanned;
STATIC B00T_DISK_FILE     *mDiskFiles;
STATIC UINTN              mDiskCount;
STATIC EFI_STATUS         mDiskStatus;

STATIC
VOID
ReadVaultState (
  IN EFI_FILE_PROTOCOL  *OcDir
  )
{
  STATIC CONST CHAR8  Magic[] = "=BEGIN OC VAULT=";
  EFI_STATUS          Status;
  EFI_FILE_PROTOCOL   *File;
  UINT8               *Efi;
  UINTN               EfiSize;
  UINTN               Offset;
  UINTN               Index;

  if (mVaultStateRead) {
    return;
  }

  mVaultStateRead = TRUE;
  Status          = OcDir->Open (OcDir, &File, B00T_VAULT_NAME, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    mVaultPlistOnDisk = TRUE;
    File->Close (File);
  }

  mVaultKeyStatus = ReadWholeFileMax (OcDir, B00T_OC_EFI_NAME, B00T_MAX_EFI_SIZE, &Efi, &EfiSize);
  if (EFI_ERROR (mVaultKeyStatus)) {
    return;
  }

  //
  // Same place sign.command patches: the 528 bytes after the start marker. All zero = no key.
  //
  for (Offset = 0; Offset + 16 + B00T_VAULT_KEY_SIZE <= EfiSize; ++Offset) {
    if (CompareMem (Efi + Offset, Magic, 16) != 0) {
      continue;
    }

    for (Index = 0; Index < B00T_VAULT_KEY_SIZE; ++Index) {
      if (Efi[Offset + 16 + Index] != 0) {
        //
        // Pool memory is 8-byte aligned, which the UINT64 key words need.
        //
        mVaultKey = AllocateCopyPool (B00T_VAULT_KEY_SIZE, Efi + Offset + 16);
        if (mVaultKey == NULL) {
          mVaultKeyStatus = EFI_OUT_OF_RESOURCES;
        }

        break;
      }
    }

    break;
  }

  FreePool (Efi);
}

/**
  Files create_vault.sh leaves out, plus the ones only this tool writes.
**/
STATIC
BOOLEAN
SkipForVault (
  IN CONST CHAR16  *Name,
  IN UINTN         Depth,
  IN BOOLEAN       Directory
  )
{
  UINTN  Length;

  if (Name[0] == L'.') {
    return TRUE;
  }

  if (Directory) {
    return Depth == 0 && B00tStriEqual (Name, L"B00tGeni3");
  }

  Length = StrLen (Name);
  if (  ((Length >= 6) && (OcStrniCmp (Name, L"vault.", 6) == 0))
     || B00tStriEqual (Name, B00T_OC_EFI_NAME)
     || B00tStriEqual (Name, L"MemTest86.log")
     || ((Length > 22) && (OcStrniCmp (Name, L"MemTest86-Report-", 17) == 0) && B00tStriEqual (Name + Length - 5, L".html")))
  {
    return TRUE;
  }

  return Depth == 0
         && (  B00tStriEqual (Name, B00T_CONFIG_NAME)
            || B00tStriEqual (Name, B00T_CONFIG_TMP)
            || B00tStriEqual (Name, B00T_CONFIG_OLD));
}

STATIC
EFI_STATUS
HashFile (
  IN  EFI_FILE_PROTOCOL  *Dir,
  IN  CONST CHAR16       *Name,
  IN  UINT8              *Buffer,
  OUT UINT8              *Hash
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  SHA256_CONTEXT     Context;
  UINTN              Size;

  Status = Dir->Open (Dir, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Sha256Init (&Context);
  do {
    Size   = SIZE_64KB;
    Status = File->Read (File, &Size, Buffer);
    if (!EFI_ERROR (Status) && (Size > 0)) {
      Sha256Update (&Context, Buffer, Size);
    }
  } while (!EFI_ERROR (Status) && (Size > 0));

  File->Close (File);
  Sha256Final (&Context, Hash);
  return Status;
}

STATIC
VOID
ScanDir (
  IN     EFI_FILE_PROTOCOL  *Dir,
  IN OUT CHAR8              *Path,
  IN     UINTN              PathLength,
  IN     UINTN              Depth,
  IN     EFI_FILE_INFO      *Info,
  IN     UINTN              InfoSize,
  IN     UINT8              *Buffer
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Sub;
  UINTN              ReadSize;
  UINTN              Index;
  UINTN              Length;
  BOOLEAN            Directory;
  B00T_DISK_FILE     *Entry;

  if (Depth > 8) {
    mDiskStatus = EFI_UNSUPPORTED;
    return;
  }

  Status = Dir->SetPosition (Dir, 0);
  while (!EFI_ERROR (Status) && !EFI_ERROR (mDiskStatus)) {
    ReadSize = InfoSize;
    Status   = Dir->Read (Dir, &ReadSize, Info);
    if (EFI_ERROR (Status) || (ReadSize == 0)) {
      break;
    }

    Directory = (Info->Attribute & EFI_FILE_DIRECTORY) != 0;
    if (  (StrCmp (Info->FileName, L".") == 0) || (StrCmp (Info->FileName, L"..") == 0)
       || SkipForVault (Info->FileName, Depth, Directory))
    {
      continue;
    }

    //
    // Vault-style path: Prefix\Name. OpenCore compares the bytes as stored, so keep the case.
    //
    Length = StrLen (Info->FileName);
    if (PathLength + Length + 2 > OC_STORAGE_SAFE_PATH_MAX) {
      mDiskStatus = EFI_BUFFER_TOO_SMALL;
      break;
    }

    for (Index = 0; Index < Length; ++Index) {
      Path[PathLength + Index] = (Info->FileName[Index] < 0x80) ? (CHAR8)Info->FileName[Index] : '?';
    }

    Path[PathLength + Length] = '\0';

    if (Directory) {
      Status = Dir->Open (Dir, &Sub, Info->FileName, EFI_FILE_MODE_READ, 0);
      if (EFI_ERROR (Status)) {
        mDiskStatus = Status;
        break;
      }

      Path[PathLength + Length]     = '\\';
      Path[PathLength + Length + 1] = '\0';
      ScanDir (Sub, Path, PathLength + Length + 1, Depth + 1, Info, InfoSize, Buffer);
      Sub->Close (Sub);
      //
      // The recursion reused Info: read on from where this level was.
      //
      continue;
    }

    if (mDiskCount == B00T_MAX_DISK_FILES) {
      mDiskStatus = EFI_BUFFER_TOO_SMALL;
      break;
    }

    Entry = &mDiskFiles[mDiskCount++];
    AsciiStrCpyS (Entry->Path, ARRAY_SIZE (Entry->Path), Path);
    Entry->Status = HashFile (Dir, Info->FileName, Buffer, Entry->Hash);
  }

  if (EFI_ERROR (Status)) {
    mDiskStatus = Status;
  }
}

STATIC
VOID
ScanOcFiles (
  IN EFI_FILE_PROTOCOL  *OcDir
  )
{
  EFI_FILE_INFO  *Info;
  UINTN          InfoSize;
  UINT8          *Buffer;
  CHAR8          Path[OC_STORAGE_SAFE_PATH_MAX];

  if (mDiskScanned) {
    return;
  }

  mDiskScanned = TRUE;
  InfoSize     = SIZE_OF_EFI_FILE_INFO + 512 * sizeof (CHAR16);
  Info         = AllocatePool (InfoSize);
  Buffer       = AllocatePool (SIZE_64KB);
  mDiskFiles   = AllocateZeroPool (B00T_MAX_DISK_FILES * sizeof (B00T_DISK_FILE));
  if ((Info == NULL) || (Buffer == NULL) || (mDiskFiles == NULL)) {
    mDiskStatus = EFI_OUT_OF_RESOURCES;
  } else {
    Path[0] = '\0';
    ScanDir (OcDir, Path, 0, 0, Info, InfoSize, Buffer);
  }

  if (Info != NULL) {
    FreePool (Info);
  }

  if (Buffer != NULL) {
    FreePool (Buffer);
  }
}

STATIC
VOID
PrintDetail (
  IN OUT UINTN        *Shown,
  IN     CONST CHAR16 *What,
  IN     CONST CHAR8  *Path
  )
{
  if (*Shown < B00T_MAX_DETAIL) {
    Print (L"  - %s: %a\n", What, Path);
  }

  ++*Shown;
}

/**
  The vault part of the pre-flight. Adds to Problems for everything that would make
  OpenCore stop before the picker once this variant (and its set) is on disk.
**/
STATIC
VOID
PreflightVault (
  IN     EFI_FILE_PROTOCOL   *OcDir,
  IN     OC_GLOBAL_CONFIG    *Config,
  IN     CONST B00T_VARIANT  *Variant,
  IN OUT UINTN               *Problems
  )
{
  CONST CHAR8       *Mode;
  BOOLEAN           NeedSet;
  OC_STORAGE_VAULT  Vault;
  UINT8             *Copy;
  UINT32            Index;
  UINTN             Disk;
  UINTN             Shown;
  BOOLEAN           ConfigFound;
  BOOLEAN           Found;
  CONST CHAR8       *Path;

  ReadVaultState (OcDir);
  Mode = OC_BLOB_GET (&Config->Misc.Security.Vault);
  Print (
    L"  vault: %a, key in OpenCore.efi: %s, OC\\vault.plist: %s, set: %s\n",
    Mode,
    mVaultKey != NULL ? L"yes" : L"no",
    mVaultPlistOnDisk ? L"yes" : L"no",
    Variant->VaultData != NULL ? (Variant->SigData != NULL ? L"vault + sig" : L"vault only") : L"none"
    );

  if (EFI_ERROR (mVaultKeyStatus)) {
    Print (L"  - cannot read OC\\%s to look for a vault key - %r\n", B00T_OC_EFI_NAME, mVaultKeyStatus);
    ++*Problems;
    return;
  }

  if (  (AsciiStrCmp (Mode, "Optional") != 0)
     && (AsciiStrCmp (Mode, "Basic") != 0)
     && (AsciiStrCmp (Mode, "Secure") != 0))
  {
    Print (L"  - Misc > Security > Vault is %a: OpenCore stops on an unknown mode\n", Mode);
    ++*Problems;
  }

  if ((AsciiStrCmp (Mode, "Secure") == 0) && (mVaultKey == NULL)) {
    Print (L"  - Vault is Secure but OpenCore.efi has no key: OpenCore would stop\n");
    ++*Problems;
  }

  //
  // OpenCore uses OC\vault.plist whenever it exists, and needs vault.sig whenever it has a key.
  //
  NeedSet = (mVaultKey != NULL) || mVaultPlistOnDisk || (AsciiStrCmp (Mode, "Optional") != 0);
  if (Variant->VaultData == NULL) {
    if (NeedSet) {
      Print (L"  - no signed vault set for this config (NAME.vault + NAME.vsig, made on the Mac);\n");
      Print (L"    the vault on disk would not match it and OpenCore would stop\n");
      ++*Problems;
    }

    return;
  }

  if (mVaultKey != NULL) {
    if ((Variant->SigData == NULL) || (Variant->SigSize != B00T_VAULT_SIG_SIZE)) {
      Print (L"  - the set's .vsig is missing or not %u bytes\n", B00T_VAULT_SIG_SIZE);
      ++*Problems;
    } else if (!RsaVerifySigDataFromKeyDynalloc (
                  mVaultKey,
                  Variant->SigData,
                  Variant->SigSize,
                  Variant->VaultData,
                  Variant->VaultSize,
                  OcSigHashTypeSha256
                  ))
    {
      Print (L"  - the set's signature does not match the key in OpenCore.efi\n");
      ++*Problems;
    }
  }

  //
  // The parser works in place: give it a copy.
  //
  Copy = AllocateCopyPool (Variant->VaultSize, Variant->VaultData);
  if (Copy == NULL) {
    Print (L"  - out of memory, cannot check the vault\n");
    ++*Problems;
    return;
  }

  OC_STORAGE_VAULT_CONSTRUCT (&Vault, sizeof (Vault));
  if (  !ParseSerialized (&Vault, &mB00tVaultSchema, Copy, (UINT32)Variant->VaultSize, NULL)
     || (Vault.Version != OC_STORAGE_VAULT_VERSION))
  {
    Print (L"  - the set's .vault is not a version %u vault.plist\n", OC_STORAGE_VAULT_VERSION);
    ++*Problems;
    OC_STORAGE_VAULT_DESTRUCT (&Vault, sizeof (Vault));
    FreePool (Copy);
    return;
  }

  ScanOcFiles (OcDir);
  if (EFI_ERROR (mDiskStatus)) {
    Print (L"  - cannot list every file under EFI\\OC - %r\n", mDiskStatus);
    ++*Problems;
  }

  ConfigFound = FALSE;
  Shown       = 0;
  for (Index = 0; Index < Vault.Files.Count; ++Index) {
    Path = OC_BLOB_GET (Vault.Files.Keys[Index]);
    if (AsciiStrCmp (Path, "config.plist") == 0) {
      ConfigFound = TRUE;
      if (CompareMem (Vault.Files.Values[Index]->Hash, Variant->Hash, SHA256_DIGEST_SIZE) != 0) {
        Print (L"  - the vault was made for a different config.plist\n");
        ++*Problems;
      }

      continue;
    }

    if (OcAsciiStrniCmp (Path, "B00tGeni3\\", 10) == 0) {
      continue;  // OpenCore never reads the variant folder
    }

    Found = FALSE;
    for (Disk = 0; Disk < mDiskCount; ++Disk) {
      if (AsciiStrCmp (mDiskFiles[Disk].Path, Path) == 0) {
        Found = TRUE;
        break;
      }
    }

    if (!Found) {
      PrintDetail (&Shown, L"in the vault, not on disk", Path);
    } else if (EFI_ERROR (mDiskFiles[Disk].Status)) {
      PrintDetail (&Shown, L"cannot read", Path);
    } else if (CompareMem (mDiskFiles[Disk].Hash, Vault.Files.Values[Index]->Hash, SHA256_DIGEST_SIZE) != 0) {
      PrintDetail (&Shown, L"changed since signing", Path);
    }
  }

  if (!ConfigFound) {
    Print (L"  - config.plist is not in the vault\n");
    ++*Problems;
  }

  for (Disk = 0; Disk < mDiskCount; ++Disk) {
    Found = FALSE;
    for (Index = 0; Index < Vault.Files.Count; ++Index) {
      if (AsciiStrCmp (mDiskFiles[Disk].Path, OC_BLOB_GET (Vault.Files.Keys[Index])) == 0) {
        Found = TRUE;
        break;
      }
    }

    if (!Found) {
      PrintDetail (&Shown, L"on disk, not in the vault", mDiskFiles[Disk].Path);
    }
  }

  if (Shown > B00T_MAX_DETAIL) {
    Print (L"  - ... and %u more\n", (UINT32)(Shown - B00T_MAX_DETAIL));
  }

  if (Shown > 0) {
    Print (L"    files under EFI\\OC changed since the set was signed: re-sign on the Mac\n");
    *Problems += Shown;
  }

  Print (L"  vault set: %u entries, %u files on disk compared\n", Vault.Files.Count, (UINT32)mDiskCount);
  OC_STORAGE_VAULT_DESTRUCT (&Vault, sizeof (Vault));
  FreePool (Copy);
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

  if (EFI_ERROR (Variant->ReadStatus)) {
    Print (L"  - cannot read the file: %r", Variant->ReadStatus);
    Print (Variant->ReadStatus == EFI_BAD_BUFFER_SIZE ? L" (larger than 1 MB)\n" : L"\n");
    return FALSE;
  }

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

  PreflightVault (OcDir, Config, Variant, &Problems);

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
  UINT8       *OldVault;
  UINTN       OldVaultSize;
  UINT8       *OldSig;
  UINTN       OldSigSize;

  OldVault = NULL;
  OldSig   = NULL;

  Status = WriteViaTemp (B00tDir, B00T_BACKUP_TMP, B00T_BACKUP_NAME, ConfigData, ConfigSize);
  Print (L"Back up config.plist -> B00tGeni3\\%s - %r\n", B00T_BACKUP_NAME, Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Variant->VaultData != NULL) {
    //
    // Keep the vault now on disk, as a backup and in memory for a roll-back.
    //
    if (!EFI_ERROR (ReadWholeFile (OcDir, B00T_VAULT_NAME, &OldVault, &OldVaultSize))) {
      Status = WriteViaTemp (B00tDir, B00T_VAULT_BAK_TMP, B00T_VAULT_BAK, OldVault, OldVaultSize);
      Print (L"Back up vault.plist -> B00tGeni3\\%s - %r\n", B00T_VAULT_BAK, Status);
      if (EFI_ERROR (Status)) {
        goto Done;
      }
    }

    if (!EFI_ERROR (ReadWholeFile (OcDir, B00T_VSIG_NAME, &OldSig, &OldSigSize))) {
      Status = WriteViaTemp (B00tDir, B00T_VSIG_BAK_TMP, B00T_VSIG_BAK, OldSig, OldSigSize);
      Print (L"Back up vault.sig -> B00tGeni3\\%s - %r\n", B00T_VSIG_BAK, Status);
      if (EFI_ERROR (Status)) {
        goto Done;
      }
    }

    Status = WriteWholeFile (OcDir, B00T_VAULT_TMP, Variant->VaultData, Variant->VaultSize);
    if (!EFI_ERROR (Status)) {
      Status = ReadAndCompare (OcDir, B00T_VAULT_TMP, Variant->VaultData, Variant->VaultSize);
    }

    Print (L"Write the set's vault to OC\\%s - %r\n", B00T_VAULT_TMP, Status);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    if (Variant->SigData != NULL) {
      Status = WriteWholeFile (OcDir, B00T_VSIG_TMP, Variant->SigData, Variant->SigSize);
      if (!EFI_ERROR (Status)) {
        Status = ReadAndCompare (OcDir, B00T_VSIG_TMP, Variant->SigData, Variant->SigSize);
      }

      Print (L"Write the set's signature to OC\\%s - %r\n", B00T_VSIG_TMP, Status);
      if (EFI_ERROR (Status)) {
        goto Done;
      }
    }
  }

  Status = WriteWholeFile (OcDir, B00T_CONFIG_TMP, Variant->Data, Variant->Size);
  if (!EFI_ERROR (Status)) {
    Status = ReadAndCompare (OcDir, B00T_CONFIG_TMP, Variant->Data, Variant->Size);
  }

  Print (L"Write %s to OC\\%s - %r\n", Variant->Name, B00T_CONFIG_TMP, Status);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = RenameFile (OcDir, B00T_CONFIG_NAME, B00T_CONFIG_OLD);
  Print (L"Rename config.plist -> %s - %r\n", B00T_CONFIG_OLD, Status);
  if (EFI_ERROR (Status)) {
    goto Done;
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
    goto Done;
  }

  if (Variant->VaultData != NULL) {
    //
    // From here until both renames are done, config.plist and the vault on disk do not match:
    // a power cut in this window can stop OpenCore before the picker.
    //
    Status = RenameFile (OcDir, B00T_VAULT_TMP, B00T_VAULT_NAME);
    if (!EFI_ERROR (Status)) {
      Status = ReadAndCompare (OcDir, B00T_VAULT_NAME, Variant->VaultData, Variant->VaultSize);
    }

    Print (L"Rename %s -> vault.plist and verify - %r\n", B00T_VAULT_TMP, Status);
    if (!EFI_ERROR (Status) && (Variant->SigData != NULL)) {
      Status = RenameFile (OcDir, B00T_VSIG_TMP, B00T_VSIG_NAME);
      if (!EFI_ERROR (Status)) {
        Status = ReadAndCompare (OcDir, B00T_VSIG_NAME, Variant->SigData, Variant->SigSize);
      }

      Print (L"Rename %s -> vault.sig and verify - %r\n", B00T_VSIG_TMP, Status);
    }

    if (EFI_ERROR (Status)) {
      //
      // Put the previous three back so they match each other again.
      //
      Print (L"Restoring previous config.plist - %r\n", RenameFile (OcDir, B00T_CONFIG_OLD, B00T_CONFIG_NAME));
      if (OldVault != NULL) {
        Print (L"Restoring previous vault.plist - %r\n", WriteViaTemp (OcDir, B00T_VAULT_TMP, B00T_VAULT_NAME, OldVault, OldVaultSize));
      } else {
        Print (L"Removing the new vault.plist - %r\n", DeleteIfExists (OcDir, B00T_VAULT_NAME));
      }

      if (OldSig != NULL) {
        Print (L"Restoring previous vault.sig - %r\n", WriteViaTemp (OcDir, B00T_VSIG_TMP, B00T_VSIG_NAME, OldSig, OldSigSize));
      }

      goto Done;
    }
  }

  Status = DeleteIfExists (OcDir, B00T_CONFIG_OLD);
  Print (L"Delete %s - %r\n", B00T_CONFIG_OLD, Status);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  UnicodeStrToAsciiStrS (Variant->Name, ActiveText, B00T_MAX_NAME);
  AsciiStrCatS (ActiveText, sizeof (ActiveText), "\n");
  Status = WriteViaTemp (B00tDir, B00T_ACTIVE_TMP, B00T_ACTIVE_NAME, (UINT8 *)ActiveText, AsciiStrLen (ActiveText));
  Print (L"Record active name in B00tGeni3\\%s - %r\n", B00T_ACTIVE_NAME, Status);

Done:
  if (OldVault != NULL) {
    FreePool (OldVault);
  }

  if (OldSig != NULL) {
    FreePool (OldSig);
  }

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
  Map a row of the full 56-line layout to the row it has on this console. The compact
  layout uses smaller logos and drops the link line and the last skull row.
**/
STATIC
UINTN
UiRow (
  IN UINTN  Row
  )
{
  if (!mUi.Compact || (Row < 20)) {
    return Row;
  }

  if (Row <= 47) {
    return Row - 5;
  }

  if ((Row == 48) || (Row == 52)) {
    return B00T_UI_HIDDEN;
  }

  return Row <= 51 ? Row - 6 : Row - 7;
}

/**
  Draw a logo given as one shade level per cell, scaled to Width x Height pixels (before
  the UI scale), centred in the layout's width and in RowSpan rows from Row down.
**/
STATIC
VOID
UiDrawArt (
  IN CONST UINT8  *Cells,
  IN UINTN        Cols,
  IN UINTN        Rows,
  IN UINTN        Row,
  IN UINTN        RowSpan,
  IN UINTN        Width,
  IN UINTN        Height,
  IN UINT8        Red,
  IN UINT8        Green,
  IN UINT8        Blue
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Buffer;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixel;
  UINTN                          X;
  UINTN                          Y;
  UINT8                          Shade;

  Width  *= mUi.Scale;
  Height *= mUi.Scale;
  Buffer  = AllocatePool (Width * Height * sizeof (*Buffer));
  if (Buffer == NULL) {
    return;
  }

  Pixel = Buffer;
  for (Y = 0; Y < Height; ++Y) {
    for (X = 0; X < Width; ++X) {
      Shade           = mShadeLevels[Cells[(Y * Rows / Height) * Cols + X * Cols / Width]];
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
             mUi.PadX + mUi.Col0 * mUi.CellWidth + (B00T_UI_COLS * mUi.CellWidth - Width) / 2,
             mUi.PadY + (mUi.Row0 + Row) * mUi.CellHeight + (RowSpan * mUi.CellHeight - Height) / 2,
             Width,
             Height,
             0
             );
  FreePool (Buffer);
}

/**
  Draw a 1 bit per pixel bitmap (most significant bit first, rows padded to a byte)
  in one color with its top left corner on a cell. Row is a row of the full layout.
**/
STATIC
VOID
UiDrawBits (
  IN CONST UINT8  *Bits,
  IN UINTN        BitsWidth,
  IN UINTN        BitsHeight,
  IN UINTN        Col,
  IN UINTN        Row,
  IN UINT8        Red,
  IN UINT8        Green,
  IN UINT8        Blue
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
  BOOLEAN                        Set;

  Row = UiRow (Row);
  if (Row == B00T_UI_HIDDEN) {
    return;
  }

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
      Set             = (Bits[(Y / mUi.Scale) * Stride + SourceX / 8] & (0x80 >> (SourceX % 8))) != 0;
      Pixel->Red      = Set ? Red : 0;
      Pixel->Green    = Set ? Green : 0;
      Pixel->Blue     = Set ? Blue : 0;
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
  Red skulls, the font has none: Count of them from Col, every Step cells.
**/
STATIC
VOID
UiDrawSkulls (
  IN UINTN  Row,
  IN UINTN  Col,
  IN UINTN  Count,
  IN UINTN  Step
  )
{
  UINTN  Index;

  for (Index = 0; Index < Count; ++Index) {
    UiDrawBits (mArtSkull, 8, 16, Col + Index * Step, Row, 0xFF, 0x00, 0x00);
  }
}

/**
  Turn text cells printed on the console's brown background into a bright yellow bar.
  Text backgrounds only come in the 8 dark colors, so the pixels are recolored in place.
**/
STATIC
VOID
UiMakeBar (
  IN UINTN  Row,
  IN UINTN  Col,
  IN UINTN  Width
  )
{
  EFI_STATUS                     Status;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Buffer;
  UINTN                          PixelWidth;
  UINTN                          Index;
  UINTN                          X;
  UINTN                          Y;

  Row = UiRow (Row);
  if (Row == B00T_UI_HIDDEN) {
    return;
  }

  PixelWidth = Width * mUi.CellWidth;
  Buffer     = AllocatePool (PixelWidth * mUi.CellHeight * sizeof (*Buffer));
  if (Buffer == NULL) {
    return;
  }

  X      = mUi.PadX + (mUi.Col0 + Col) * mUi.CellWidth;
  Y      = mUi.PadY + (mUi.Row0 + Row) * mUi.CellHeight;
  Status = mUi.Gop->Blt (mUi.Gop, Buffer, EfiBltVideoToBltBuffer, X, Y, 0, 0, PixelWidth, mUi.CellHeight, 0);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < PixelWidth * mUi.CellHeight; ++Index) {
      if ((Buffer[Index].Red == 0x98) && (Buffer[Index].Green == 0x98) && (Buffer[Index].Blue == 0x00)) {
        Buffer[Index].Red   = 0xFF;
        Buffer[Index].Green = 0xFF;
      }
    }

    mUi.Gop->Blt (mUi.Gop, Buffer, EfiBltBufferToVideo, 0, 0, X, Y, PixelWidth, mUi.CellHeight, 0);
  }

  FreePool (Buffer);
}

//
// The main screen (Phase 8c, layout redone in 8d from draft A2). Arrow keys move between the
// selectable items, Enter runs the selected one, Esc leaves. Sub-screens are plain text pages.
//
typedef enum {
  B00tUiStay,
  B00tUiExit,
  B00tUiReset
} B00T_UI_RESULT;

typedef struct {
  BOOLEAN    Pass;                        ///< Pre-flight result.
  CHAR16     Serial[32];
  CHAR16     BootArgs[B00T_MAX_ARGS];
} B00T_VARIANT_INFO;

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
  CHAR16                       EditArgs[B00T_MAX_ARGS];      ///< The boot-arg line being edited.
  B00T_VARIANT_INFO            Info[B00T_MAX_VARIANTS];
  UINT8                        *OwnedConfig;                  ///< config.plist data after a COMMIT.
  UINTN                        Cursor;                        ///< Variant under the list cursor.
  UINTN                        Top;                           ///< First variant shown in the list.
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
  Print Text at a position of the full layout, cut or padded with spaces to exactly Width cells.
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

  Row = UiRow (Row);
  if (Row == B00T_UI_HIDDEN) {
    return;
  }

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

#define B00T_UI_HINT_ROW     53
#define B00T_UI_LIST_ROW     35
#define B00T_UI_LIST_ROWS    5
#define B00T_UI_FIELD_ROW    28
#define B00T_UI_FIELD_COL    18
#define B00T_UI_FIELD_WIDTH  70

typedef enum {
  B00T_SEL_VERBOSE,
  B00T_SEL_SAFE,
  B00T_SEL_SINGLE,
  B00T_SEL_DEBUG,
  B00T_SEL_ARGS_LIST,
  B00T_SEL_EXTRAS,
  B00T_SEL_FIELD,
  B00T_SEL_COMMIT,
  B00T_SEL_APPLY,
  B00T_SEL_ONCE,
  B00T_SEL_DISCARD,
  B00T_SEL_LIST,
  B00T_SEL_RESTORE,
  B00T_SEL_KEXT,
  B00T_SEL_NVRAM,
  B00T_SEL_SIP,
  B00T_SEL_DISKMAP,
  B00T_SEL_SSDT,
  B00T_SEL_SHELL,
  B00T_SEL_COUNT
} B00T_SEL;

typedef struct {
  UINT8           Row;
  UINT8           Col;
  CONST CHAR16    *Text;
  CONST CHAR16    *Hint;
} B00T_UI_SEL;

//
// Selectable items at their place in the full layout. B00T_SEL_LIST is the variant list:
// its row is the row of the list cursor.
//
STATIC CONST B00T_UI_SEL  mUiSel[B00T_SEL_COUNT] = {
  { 24,   9, L"[ ] VERBOSE (-v)",             L"ENTER adds or removes -v in the boot-arg line"                                     },
  { 24,  36, L"[ ] SAFE BOOT (-x)",           L"ENTER adds or removes -x in the boot-arg line"                                     },
  { 24,  64, L"[ ] SINGLE USER (-s)",         L"ENTER adds or removes -s in the boot-arg line"                                     },
  { 24,  92, L"[ ] DEBUG (debug=0x100)",      L"ENTER adds or removes debug=0x100 in the boot-arg line"                            },
  { 26,  30, L"[***ADDITIONAL-ARGS-LIST***]", L"Pick common boot-args to add to or remove from the boot-arg line"                  },
  { 26,  72, L"[ EXTRAS/UTILITIES ]",         L"EXTRAS/UTILITIES - coming soon"                                                    },
  { 28,   9, L"boot-arg=",                    L"ENTER to type in the boot-arg line"                                                },
  { 28,  88, L"::COMMIT",                     L"Save the boot-arg line into config.plist, no reset; OpenCore uses it from the next boot" },
  { 30,  16, L"[ APPLY & REBOOT ]",           L"Save the boot-arg line into config.plist, then cold reset"                         },
  { 30,  44, L"[ B00T ONCE ]",                L"Set NVRAM boot-args for this boot only and go back to the picker"                  },
  { 30,  78, L"[DISCARD CHANGES AND EXIT]",   L"Return to the OpenCore picker, nothing changed"                                    },
  { 35,   5, L"",                             L"ENTER runs the pre-flight on this config, you confirm once, it is swapped in, then cold reset" },
  { 41,   7, L"RESTORE-FAILSAFE",             L"Swap known-good.plist into config.plist, then cold reset"                          },
  { 42,   7, L"KEXT CONTROL",                 L"KEXT CONTROL - coming soon"                                                        },
  { 45,   9, L"NVRAM-RESET",                  L"Delete NVRAM variables with CleanNvram.efi, then cold reset"                       },
  { 45,  31, L"DISABLE SIP - ENABLE SIP",     L"Toggle csr-active-config with CsrUtil.efi"                                         },
  { 45,  64, L"DUMP-DISKMAP",                 L"List block devices and file systems with their device paths"                       },
  { 45,  85, L"DUMP SSDT",                    L"Write the ACPI tables to a dated folder inside EFI/OC/B00tGeni3"                   },
  { 45, 103, L"UEFI SHELL",                   L"Start OpenShell.efi; type exit there to come back"                                 },
};

//
// Boot-args behind the four checkboxes, in B00T_SEL order.
//
STATIC CONST CHAR16  *mCheckTokens[] = { L"-v", L"-x", L"-s", L"debug=0x100" };

/**
  Look for Token as a whole space separated word in Args.
**/
STATIC
BOOLEAN
ArgsFind (
  IN  CONST CHAR16  *Args,
  IN  CONST CHAR16  *Token,
  OUT UINTN         *Start  OPTIONAL
  )
{
  UINTN  Index;
  UINTN  Begin;
  UINTN  Length;

  Length = StrLen (Token);
  Index  = 0;
  while (Args[Index] != L'\0') {
    while (Args[Index] == L' ') {
      ++Index;
    }

    Begin = Index;
    while ((Args[Index] != L'\0') && (Args[Index] != L' ')) {
      ++Index;
    }

    if ((Index - Begin == Length) && (Length > 0) && (StrnCmp (&Args[Begin], Token, Length) == 0)) {
      if (Start != NULL) {
        *Start = Begin;
      }

      return TRUE;
    }
  }

  return FALSE;
}

/**
  Remove Token from Args if it is there, otherwise append it. Args holds B00T_MAX_ARGS characters.
**/
STATIC
VOID
ArgsToggle (
  IN OUT CHAR16        *Args,
  IN     CONST CHAR16  *Token
  )
{
  UINTN  Start;
  UINTN  End;
  UINTN  Used;
  UINTN  Length;

  Length = StrLen (Token);
  if (ArgsFind (Args, Token, &Start)) {
    End = Start + Length;
    if (Args[End] == L' ') {
      ++End;
    } else if (Start > 0) {
      --Start;
    }

    CopyMem (&Args[Start], &Args[End], (StrLen (&Args[End]) + 1) * sizeof (CHAR16));
    return;
  }

  Used = StrLen (Args);
  if (Used + Length + 2 > B00T_MAX_ARGS) {
    return;
  }

  if ((Used > 0) && (Args[Used - 1] != L' ')) {
    StrCatS (Args, B00T_MAX_ARGS, L" ");
  }

  StrCatS (Args, B00T_MAX_ARGS, Token);
}

/**
  Show Text centred on the hint line.
**/
STATIC
VOID
UiStatus (
  IN UINTN         Attribute,
  IN CONST CHAR16  *Text
  )
{
  UINTN  Length;

  Length = MIN (StrLen (Text), B00T_UI_COLS - 4);
  UiPutText (B00T_UI_HINT_ROW, 2, B00T_UI_COLS - 4, Attribute, L"");
  UiPutText (B00T_UI_HINT_ROW, (B00T_UI_COLS - Length) / 2, Length, Attribute, Text);
}

STATIC
VOID
UiDrawSel (
  IN CONST B00T_CONTEXT  *Context,
  IN UINTN               Index,
  IN BOOLEAN             Selected
  )
{
  CHAR16  Text[48];
  UINTN   Color;

  if (Index == B00T_SEL_LIST) {
    return;
  }

  StrCpyS (Text, ARRAY_SIZE (Text), mUiSel[Index].Text);
  if ((Index <= B00T_SEL_DEBUG) && ArgsFind (Context->EditArgs, mCheckTokens[Index], NULL)) {
    Text[1] = L'x';
  }

  Color = ((Index == B00T_SEL_KEXT) || (Index == B00T_SEL_EXTRAS)) ? EFI_LIGHTGRAY : EFI_WHITE;
  UiPutText (
    mUiSel[Index].Row,
    mUiSel[Index].Col,
    StrLen (Text),
    EFI_TEXT_ATTR (Selected ? EFI_YELLOW : Color, EFI_BLACK),
    Text
    );
}

/**
  Find the item an arrow key leads to: the nearest row in that direction, then the
  nearest centre; left and right stay on the row. Returns Current if there is none.
**/
STATIC
UINTN
UiNeighbour (
  IN CONST B00T_CONTEXT  *Context,
  IN UINTN               Current,
  IN UINT16              ScanCode
  )
{
  UINTN  Index;
  UINTN  Best;
  UINTN  BestScore;
  UINTN  Score;
  UINTN  Row[B00T_SEL_COUNT];
  UINTN  Middle[B00T_SEL_COUNT];

  for (Index = 0; Index < B00T_SEL_COUNT; ++Index) {
    Row[Index]    = mUiSel[Index].Row;
    Middle[Index] = mUiSel[Index].Col * 2 + StrLen (mUiSel[Index].Text);
  }

  Row[B00T_SEL_LIST]    = B00T_UI_LIST_ROW + Context->Cursor - Context->Top;
  Middle[B00T_SEL_LIST] = mUiSel[B00T_SEL_LIST].Col * 2 + 40;

  Best      = Current;
  BestScore = MAX_UINTN;
  for (Index = 0; Index < B00T_SEL_COUNT; ++Index) {
    if (  (Index == Current)
       || ((Index == B00T_SEL_LIST) && (Context->Count == 0))
       || ((ScanCode == SCAN_UP) && (Row[Index] >= Row[Current]))
       || ((ScanCode == SCAN_DOWN) && (Row[Index] <= Row[Current]))
       || ((ScanCode == SCAN_LEFT) && ((Row[Index] != Row[Current]) || (Middle[Index] >= Middle[Current])))
       || ((ScanCode == SCAN_RIGHT) && ((Row[Index] != Row[Current]) || (Middle[Index] <= Middle[Current]))))
    {
      continue;
    }

    Score = (Row[Index] > Row[Current] ? Row[Index] - Row[Current] : Row[Current] - Row[Index]) * 1000
            + (Middle[Index] > Middle[Current] ? Middle[Index] - Middle[Current] : Middle[Current] - Middle[Index]);
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
  Read the serial number and the boot-args string out of a config.plist held in memory.

  @retval FALSE  OpenCore's parser rejects the data; both strings are empty.
**/
STATIC
BOOLEAN
ParseConfigInfo (
  IN  CONST UINT8  *Data,
  IN  UINTN        Size,
  OUT CHAR16       *Serial,
  IN  UINTN        SerialCount,
  OUT CHAR16       *BootArgs,
  IN  UINTN        BootArgsCount
  )
{
  EFI_STATUS        Status;
  OC_GLOBAL_CONFIG  *Config;
  UINT8             *Copy;
  UINT32            ErrorCount;
  UINT32            Index;
  UINT32            Index2;
  OC_ASSOC          *Map;

  Serial[0]   = L'\0';
  BootArgs[0] = L'\0';

  Copy = AllocateCopyPool (Size, Data);
  if (Copy == NULL) {
    return FALSE;
  }

  Config     = &mPreflightConfig;
  ErrorCount = 0;
  Status     = OcConfigurationInit (Config, Copy, (UINT32)Size, &ErrorCount);
  if (EFI_ERROR (Status)) {
    FreePool (Copy);
    return FALSE;
  }

  AsciiToUnicodeN (
    OC_BLOB_GET (&Config->PlatformInfo.Generic.SystemSerialNumber),
    Config->PlatformInfo.Generic.SystemSerialNumber.Size,
    Serial,
    SerialCount
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
          BootArgs,
          BootArgsCount
          );
      }
    }
  }

  OcConfigurationFree (Config);
  FreePool (Copy);
  return TRUE;
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

typedef struct {
  UINT8           Row;
  UINT8           Col;
  UINT8           Color;
  CONST CHAR16    *Text;
} B00T_UI_LABEL;

//
// Fixed text of the screen, at its place in the full layout.
//
STATIC CONST B00T_UI_LABEL  mUiLabels[] = {
  {  1,  3, EFI_LIGHTGRAY, L"BOOT DEVICE:"                                                                         },
  {  1, 46, EFI_LIGHTGRAY, L"MAC:"                                                                                 },
  {  1, 86, EFI_LIGHTGRAY, L"SERIAL:"                                                                              },
  {  2,  3, EFI_LIGHTGRAY, L"SIP LEVEL:"                                                                           },
  {  2, 46, EFI_LIGHTGRAY, L"B00t ARGS:"                                                                           },
  {  2, 86, EFI_LIGHTGRAY, L"ACTIVE:"                                                                              },
  { 27,  9, EFI_LIGHTGRAY, L"CURRENT CONFIG BOOT-ARGs:"                                                            },
  { 33,  7, EFI_LIGHTGRAY, L"VARIANT                    STATE      CHECK"                                          },
  { 39, 64, EFI_LIGHTGRAY, L"Compared with the active config"                                                      },
  { 46,  9, EFI_LIGHTRED,  L"wipes boot list"                                                                      },
  { 48, 45, EFI_LIGHTCYAN, L"https://github.com/a7ent/B00tGeni3"                                                   },
  { 54, 23, EFI_LIGHTGRAY, L"\x2190 \x2191 \x2193 \x2192  move        ENTER  select        ESC  back to the OpenCore picker" },
};

STATIC
VOID
UiHLine (
  IN UINTN  Row,
  IN UINTN  Col0,
  IN UINTN  Col1,
  IN UINTN  Attribute
  )
{
  CHAR16  Line[B00T_UI_COLS + 1];
  UINTN   Index;

  for (Index = 0; Index + Col0 <= Col1; ++Index) {
    Line[Index] = BOXDRAW_HORIZONTAL;
  }

  Line[Index] = L'\0';
  UiPutText (Row, Col0, Index, Attribute, Line);
}

/**
  Draw a box frame with a title on its top edge. The inside is left alone.
**/
STATIC
VOID
UiBox (
  IN UINTN         Row0,
  IN UINTN         Col0,
  IN UINTN         Row1,
  IN UINTN         Col1,
  IN UINTN         Attribute,
  IN BOOLEAN       Double,
  IN CONST CHAR16  *Title,
  IN UINTN         TitleAttribute
  )
{
  CHAR16  Line[B00T_UI_COLS + 1];
  UINTN   Width;
  UINTN   Index;

  Width = Col1 - Col0 + 1;
  for (Index = 0; Index < Width; ++Index) {
    Line[Index] = Double ? BOXDRAW_DOUBLE_HORIZONTAL : BOXDRAW_HORIZONTAL;
  }

  Line[Width]     = L'\0';
  Line[0]         = Double ? BOXDRAW_DOUBLE_DOWN_RIGHT : BOXDRAW_DOWN_RIGHT;
  Line[Width - 1] = Double ? BOXDRAW_DOUBLE_DOWN_LEFT : BOXDRAW_DOWN_LEFT;
  UiPutText (Row0, Col0, Width, Attribute, Line);
  Line[0]         = Double ? BOXDRAW_DOUBLE_UP_RIGHT : BOXDRAW_UP_RIGHT;
  Line[Width - 1] = Double ? BOXDRAW_DOUBLE_UP_LEFT : BOXDRAW_UP_LEFT;
  UiPutText (Row1, Col0, Width, Attribute, Line);

  Line[0] = Double ? BOXDRAW_DOUBLE_VERTICAL : BOXDRAW_VERTICAL;
  Line[1] = L'\0';
  for (Index = Row0 + 1; Index < Row1; ++Index) {
    UiPutText (Index, Col0, 1, Attribute, Line);
    UiPutText (Index, Col1, 1, Attribute, Line);
  }

  UnicodeSPrint (Line, sizeof (Line), L" %s ", Title);
  UiPutText (Row0, Col0 + 2, MIN (StrLen (Line), Width - 4), TitleAttribute, Line);
}

/**
  Draw everything on the screen that never changes: logos, skulls, frames, labels, banner.
  The compact layout uses smaller logos and drops the link line and the last skull row.
**/
STATIC
VOID
UiDrawMain (
  VOID
  )
{
  STATIC CONST CHAR16  *Tagline = L"..I'm never gonna push you out, cause you're not in..";
  STATIC CONST CHAR16  *Title   = L"~~ B00tGeni3   2026 ~~          By:  oTR-DaRkNeSs  x  CL4UD3-C0D3 ";
  UINTN                Index;
  UINTN                White;
  UINTN                Green;
  UINTN                LastLogoRow;

  UiBlackout ();
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  White = EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK);
  Green = EFI_TEXT_ATTR (EFI_LIGHTGREEN, EFI_BLACK);

  //
  // Logos first: the tagline is text printed over cells inside the red logo.
  //
  if (mUi.Compact) {
    UiDrawArt (&mArtOtr[0][0], B00T_ART_OTR_COLS, B00T_ART_OTR_ROWS, 5, 6, 450, 96, 0xFF, 0x00, 0x00);
    UiDrawArt (&mArtBig[0][0], B00T_ART_BIG_COLS, B00T_ART_BIG_ROWS, 11, 4, 448, 56, 0x00, 0xFF, 0x00);
    LastLogoRow = 14;
  } else {
    UiDrawArt (&mArtOtr[0][0], B00T_ART_OTR_COLS, B00T_ART_OTR_ROWS, 5, 8, 600, 128, 0xFF, 0x00, 0x00);
    UiDrawArt (&mArtBig[0][0], B00T_ART_BIG_COLS, B00T_ART_BIG_ROWS, 13, 7, 896, 112, 0x00, 0xFF, 0x00);
    LastLogoRow = 19;
  }

  UiPutText (
    mUi.Compact ? 9 : 10,
    (B00T_UI_COLS - StrLen (Tagline)) / 2,
    StrLen (Tagline),
    EFI_TEXT_ATTR (EFI_LIGHTRED, EFI_BLACK),
    Tagline
    );

  UiDrawSkulls (4, 0, B00T_UI_COLS / 2, 2);
  UiDrawSkulls (20, 0, B00T_UI_COLS / 2, 2);
  UiDrawSkulls (22, 0, B00T_UI_COLS / 2, 2);
  UiDrawSkulls (52, 0, B00T_UI_COLS / 2, 2);
  for (Index = 5; Index <= LastLogoRow; ++Index) {
    UiDrawSkulls (Index, 0, 2, 2);
    UiDrawSkulls (Index, 120, 2, 2);
  }

  //
  // Title line: the skull between the name and the year is a bitmap on the blank cell.
  //
  UiPutText (21, 24, StrLen (Title), EFI_TEXT_ATTR (EFI_LIGHTRED, EFI_BLACK), Title);
  UiDrawSkulls (21, 24 + 13, 1, 1);
  UiPutText (21, 24 + StrLen (Title), 5, Green, L"<cc/>");

  UiBox (0, 0, 3, 123, EFI_TEXT_ATTR (EFI_LIGHTRED, EFI_BLACK), TRUE, L"**SYSTEM INFO::**", Green);
  UiBox (23, 3, 31, 120, White, FALSE, L"BOOT-ARGS", Green);
  UiBox (32, 3, 43, 58, White, FALSE, L"CONFIGS", Green);
  UiBox (44, 3, 47, 120, White, FALSE, L"B00T-TOOLS", Green);
  UiHLine (34, 4, 57, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
  UiHLine (40, 4, 57, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));

  for (Index = 0; Index < ARRAY_SIZE (mUiLabels); ++Index) {
    UiPutText (
      mUiLabels[Index].Row,
      mUiLabels[Index].Col,
      StrLen (mUiLabels[Index].Text),
      EFI_TEXT_ATTR (mUiLabels[Index].Color, EFI_BLACK),
      mUiLabels[Index].Text
      );
  }

  UiDrawBits (mArtBanner, B00T_ART_BANNER_WIDTH, B00T_ART_BANNER_HEIGHT, 8, 49, 0xFF, 0xFF, 0xFF);
}

/**
  Fill in the system info box. Called on every redraw, so a SIP toggle shows up
  when its page closes.
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
  CONST CHAR16                 *Active;
  UINTN                        Size;
  UINTN                        Index;
  UINT32                       Csr;
  UINTN                        Value;

  Value = EFI_TEXT_ATTR (EFI_LIGHTGREEN, EFI_BLACK);

  BootDeviceText (Context->LoadedImage->DeviceHandle, Text, sizeof (Text));
  UiPutText (1, 16, 28, Value, Text);

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

  UiPutText (1, 51, 30, Value, Text);
  UiPutText (1, 94, 28, Value, Context->Serial[0] != L'\0' ? Context->Serial : L"n/a");

  Size   = sizeof (Csr);
  Status = gRT->GetVariable (L"csr-active-config", &gAppleBootVariableGuid, NULL, &Size, &Csr);
  if (!EFI_ERROR (Status) && (Size == sizeof (Csr))) {
    UnicodeSPrint (Text, sizeof (Text), L"0x%08X", Csr);
  } else {
    StrCpyS (Text, ARRAY_SIZE (Text), L"not set");
  }

  UiPutText (2, 16, 28, Value, Text);

  Size   = sizeof (Args) - 1;
  Status = gRT->GetVariable (L"boot-args", &gAppleBootVariableGuid, NULL, &Size, Args);
  if (!EFI_ERROR (Status)) {
    AsciiToUnicodeN (Args, Size, Text, ARRAY_SIZE (Text));
  } else {
    StrCpyS (Text, ARRAY_SIZE (Text), L"not set");
  }

  UiPutText (2, 57, 27, Value, Text);

  Active = L"(matches no variant)";
  for (Index = 0; Index < Context->Count; ++Index) {
    if (Context->Variants[Index].Active) {
      Active = Context->Variants[Index].Name;
    }
  }

  UiPutText (2, 94, 28, Value, Active);
}

/**
  Draw the four checkboxes, the boot-arg line and its "edited" note.
**/
STATIC
VOID
UiDrawArgs (
  IN CONST B00T_CONTEXT  *Context,
  IN BOOLEAN             Editing,
  IN UINTN               Selected
  )
{
  CHAR16  Text[B00T_MAX_ARGS + 1];
  UINTN   Index;
  UINTN   Length;
  BOOLEAN Edited;

  for (Index = B00T_SEL_VERBOSE; Index <= B00T_SEL_DEBUG; ++Index) {
    UiDrawSel (Context, Index, Index == Selected);
  }

  //
  // A line longer than the field shows its end, which is where typing happens.
  //
  UnicodeSPrint (Text, sizeof (Text), L"%s%s", Context->EditArgs, Editing ? L"_" : L"");
  Length = StrLen (Text);
  UiPutText (
    B00T_UI_FIELD_ROW,
    B00T_UI_FIELD_COL,
    B00T_UI_FIELD_WIDTH,
    EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY),
    &Text[Length > B00T_UI_FIELD_WIDTH ? Length - B00T_UI_FIELD_WIDTH : 0]
    );

  Edited = StrCmp (Context->EditArgs, Context->ConfigBootArgs) != 0;
  UiPutText (27, 36, 30, EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK), Edited ? L"edited, not saved" : L"");
}

/**
  Draw the variant list. With Focus the cursor line is a yellow bar.
**/
STATIC
VOID
UiDrawList (
  IN CONST B00T_CONTEXT  *Context,
  IN BOOLEAN             Focus
  )
{
  CONST B00T_VARIANT  *Variant;
  CONST CHAR16        *State;
  CHAR16              Name[28];
  CHAR16              Text[64];
  UINTN               Line;
  UINTN               Index;
  UINTN               Row;
  BOOLEAN             Pass;

  for (Line = 0; Line < B00T_UI_LIST_ROWS; ++Line) {
    Index = Context->Top + Line;
    Row   = B00T_UI_LIST_ROW + Line;
    if (Index >= Context->Count) {
      UiPutText (
        Row,
        4,
        54,
        EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK),
        (Line == 0) && (Context->Count == 0) ? L"   no config variants in " B00T_DIR_PATH : L""
        );
      continue;
    }

    Variant = &Context->Variants[Index];
    Pass    = Context->Info[Index].Pass;
    State   = Variant->Active ? L"ACTIVE" : (B00tStriEqual (Variant->Name, B00T_KNOWN_GOOD) ? L"FAILSAFE" : L"");
    StrnCpyS (Name, ARRAY_SIZE (Name), Variant->Name, ARRAY_SIZE (Name) - 2);

    if ((Index == Context->Cursor) && Focus) {
      UnicodeSPrint (Text, sizeof (Text), L" > %-27s%-11s%s", Name, State, Pass ? L"PASS" : L"REFUSED");
      UiPutText (Row, 4, 54, EFI_TEXT_ATTR (EFI_BLACK, EFI_BROWN), Text);
      UiMakeBar (Row, 4, 54);
      continue;
    }

    UiPutText (Row, 4, 3, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK), Index == Context->Cursor ? L" >" : L"");
    UiPutText (Row, 7, 27, EFI_TEXT_ATTR (Pass ? EFI_WHITE : EFI_LIGHTGRAY, EFI_BLACK), Name);
    UiPutText (Row, 34, 11, EFI_TEXT_ATTR (Variant->Active ? EFI_LIGHTGREEN : EFI_LIGHTCYAN, EFI_BLACK), State);
    UiPutText (Row, 45, 13, EFI_TEXT_ATTR (Pass ? EFI_LIGHTGREEN : EFI_LIGHTRED, EFI_BLACK), Pass ? L"PASS" : L"REFUSED");
  }
}

/**
  Draw the box about the variant under the list cursor.
**/
STATIC
VOID
UiDrawOverview (
  IN CONST B00T_CONTEXT  *Context
  )
{
  CONST B00T_VARIANT       *Variant;
  CONST B00T_VARIANT_INFO  *Info;
  CHAR16                   Text[B00T_MAX_ARGS + 16];
  UINTN                    Gray;
  UINTN                    White;
  UINTN                    Row;
  BOOLEAN                  Same;

  Gray  = EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK);
  White = EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK);

  if (Context->Count == 0) {
    UiBox (32, 61, 43, 120, White, FALSE, L"no variant", EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK));
    for (Row = 33; Row <= 42; ++Row) {
      UiPutText (Row, 62, 58, Gray, L"");
    }

    return;
  }

  Variant = &Context->Variants[Context->Cursor];
  Info    = &Context->Info[Context->Cursor];
  UiBox (32, 61, 43, 120, White, FALSE, Variant->Name, EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK));
  UiHLine (38, 62, 119, Gray);
  UiPutText (39, 64, 31, Gray, L"Compared with the active config");

  UiPutText (33, 64, 13, Gray, L"Pre-flight");
  UiPutText (33, 77, 42, EFI_TEXT_ATTR (Info->Pass ? EFI_LIGHTGREEN : EFI_LIGHTRED, EFI_BLACK), Info->Pass ? L"PASS" : L"REFUSED");

  UnicodeSPrint (Text, sizeof (Text), L"%u bytes", (UINT32)Variant->Size);
  UiPutText (34, 64, 13, Gray, L"Size");
  UiPutText (34, 77, 42, White, Text);

  UnicodeSPrint (
    Text,
    sizeof (Text),
    L"%02x%02x%02x%02x ... %02x%02x%02x%02x",
    Variant->Hash[0],
    Variant->Hash[1],
    Variant->Hash[2],
    Variant->Hash[3],
    Variant->Hash[SHA256_DIGEST_SIZE - 4],
    Variant->Hash[SHA256_DIGEST_SIZE - 3],
    Variant->Hash[SHA256_DIGEST_SIZE - 2],
    Variant->Hash[SHA256_DIGEST_SIZE - 1]
    );
  UiPutText (35, 64, 13, Gray, L"SHA-256");
  UiPutText (35, 77, 42, White, Text);

  UiPutText (36, 64, 13, Gray, L"State");
  UiPutText (
    36,
    77,
    42,
    White,
    Variant->Active ? L"active" : (B00tStriEqual (Variant->Name, B00T_KNOWN_GOOD) ? L"failsafe" : L"-")
    );
  UiPutText (37, 62, 58, Gray, L"");

  Same = StrCmp (Info->BootArgs, Context->ConfigBootArgs) == 0;
  UiPutText (40, 64, 13, Gray, L"boot-args");
  UiPutText (40, 77, 42, Same ? White : EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK), Info->BootArgs[0] != L'\0' ? Info->BootArgs : L"(none)");

  Same = StrCmp (Info->Serial, Context->Serial) == 0;
  UnicodeSPrint (Text, sizeof (Text), L"%s  %s", Info->Serial[0] != L'\0' ? Info->Serial : L"(none)", Same ? L"same" : L"differs");
  UiPutText (41, 64, 13, Gray, L"serial");
  UiPutText (41, 77, 42, Same ? White : EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK), Text);

  if (!Info->Pass) {
    UiPutText (42, 64, 55, EFI_TEXT_ATTR (EFI_LIGHTRED, EFI_BLACK), L"ENTER shows what the pre-flight found");
  } else if (Variant->Active) {
    UiPutText (42, 64, 55, Gray, L"this is the active config");
  } else {
    UiPutText (42, 64, 55, EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK), L"ENTER  swap it in, then cold reset");
  }
}

STATIC
EFI_STATUS
EFIAPI
UiNullOutputString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  return EFI_SUCCESS;
}

/**
  Run the unchanged Phase 5 pre-flight on every variant with its text output switched off,
  and read each variant's serial number and boot-args for the list and the overview box.
**/
STATIC
VOID
UiLoadInfo (
  IN OUT B00T_CONTEXT  *Context
  )
{
  EFI_TEXT_STRING  Saved;
  UINTN            Index;

  Saved                      = gST->ConOut->OutputString;
  gST->ConOut->OutputString = UiNullOutputString;
  for (Index = 0; Index < Context->Count; ++Index) {
    Context->Info[Index].Pass = PreflightVariant (Context->OcDir, &Context->Variants[Index]);
  }

  gST->ConOut->OutputString = Saved;

  for (Index = 0; Index < Context->Count; ++Index) {
    ParseConfigInfo (
      Context->Variants[Index].Data,
      Context->Variants[Index].Size,
      Context->Info[Index].Serial,
      ARRAY_SIZE (Context->Info[Index].Serial),
      Context->Info[Index].BootArgs,
      ARRAY_SIZE (Context->Info[Index].BootArgs)
      );
  }
}

/**
  Type in the boot-arg line. Enter keeps the text, Esc puts the previous text back.
  Characters that would need escaping in the plist are not accepted.
**/
STATIC
VOID
UiEditField (
  IN OUT B00T_CONTEXT  *Context
  )
{
  EFI_INPUT_KEY  Key;
  CHAR16         Saved[B00T_MAX_ARGS];
  UINTN          Length;

  StrCpyS (Saved, ARRAY_SIZE (Saved), Context->EditArgs);
  UiStatus (EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK), L"Type the boot-args.  ENTER = done   ESC = undo   BACKSPACE = delete");
  while (TRUE) {
    UiDrawArgs (Context, TRUE, B00T_SEL_FIELD);
    Key    = WaitForKey ();
    Length = StrLen (Context->EditArgs);
    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      break;
    }

    if (Key.ScanCode == SCAN_ESC) {
      StrCpyS (Context->EditArgs, ARRAY_SIZE (Context->EditArgs), Saved);
      break;
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE) {
      if (Length > 0) {
        Context->EditArgs[Length - 1] = L'\0';
      }

      continue;
    }

    if (  (Key.UnicodeChar >= 0x20)
       && (Key.UnicodeChar < 0x7F)
       && (Key.UnicodeChar != L'<')
       && (Key.UnicodeChar != L'>')
       && (Key.UnicodeChar != L'&')
       && (Length + 2 < B00T_MAX_ARGS))
    {
      Context->EditArgs[Length]     = Key.UnicodeChar;
      Context->EditArgs[Length + 1] = L'\0';
    }
  }
}

/**
  ADDITIONAL-ARGS-LIST: choose common boot-args to add to or remove from the line.
**/
STATIC
VOID
UiArgsList (
  IN OUT B00T_CONTEXT  *Context
  )
{
  //
  // From the Dortania install guide and the acidanthera kext documentation, read 2026-11-16.
  //
  STATIC CONST CHAR16  *Common[][2] = {
    { L"keepsyms=1", L"print symbols on a kernel panic (goes with debug=0x100)" },
    { L"npci=0x3000", L"boot stalls at PCI configuration begin and there is no Above 4G option" },
    { L"alcid=1", L"AppleALC audio layout-id 1 (type another id in the line)" },
    { L"alcverbs=1", L"AppleALC alc-verb support" },
    { L"-alcoff", L"disable AppleALC" },
    { L"-wegnoegpu", L"disable all external GPUs, keep the iGPU" },
    { L"-wegnoigpu", L"disable the internal GPU" },
    { L"-wegswitchgpu", L"disable the iGPU when an external GPU is installed" },
    { L"-wegoff", L"disable WhateverGreen" },
    { L"-wegbeta", L"WhateverGreen on unsupported macOS versions" },
    { L"agdpmod=pikera", L"board-id to board-ix: Navi RX 5000/6000 black screen" },
    { L"agdpmod=vit9696", L"disable the board-id check" },
    { L"agdpmod=ignore", L"disable the AGDP patches" },
    { L"-igfxvesa", L"disable Intel graphics acceleration" },
    { L"-radvesa", L"disable AMD video acceleration" },
    { L"-igfxblr", L"backlight registers fix, KBL/CFL/ICL" },
    { L"-igfxblt", L"backlight fix alternative, KBL/CFL on macOS 13.4+" },
    { L"-igfxbls", L"smoother brightness transitions, IVB and newer" },
    { L"-igfxnotelemetryload", L"skip iGPU telemetry (startup freeze on some laptops)" },
    { L"igfxonln=1", L"force online status on all displays" },
    { L"igfxfw=2", L"force loading of the Apple GuC firmware" },
    { L"-igfxmlr", L"maximum link rate fix" },
    { L"-igfxmpc", L"raise the max pixel clock" },
    { L"-cdfon", L"HDMI 2.0 patches (not implemented for macOS 11+)" },
    { L"-radcodec", L"hardware video encoder on spoofed AMD GPUs" },
    { L"radpg=15", L"power-gating modes off, AMD Cape Verde GPUs" },
    { L"unfairgva=1", L"hardware DRM on supported AMD GPUs" },
    { L"nvda_drv_vrl=1", L"NVIDIA Web Drivers, macOS Sierra and High Sierra" },
    { L"-liluoff", L"disable Lilu (and with it its plugins)" },
    { L"-lilubetaall", L"Lilu and all plugins on unsupported OS versions" },
    { L"-liludbgall", L"debug printing, DEBUG kexts only" },
    { L"-revoff", L"disable RestrictEvents" },
    { L"revpatch=sbvmm", L"VMM model so OTA updates work on unsupported models" },
    { L"-brcmfxoff", L"disable AirportBrcmFixup" },
    { L"brcmfx-country=#a", L"Broadcom Wi-Fi country code #a" },
    { L"-nvmefoff", L"disable NVMeFix" }
  };
  CONST CHAR16         *Names[ARRAY_SIZE (Common)];
  CHAR16               Lines[ARRAY_SIZE (Common)][112];
  UINTN                Index;
  INTN                 Choice;

  Choice = 0;
  while (TRUE) {
    UiPageBegin (L"ADDITIONAL-ARGS-LIST");
    Print (L"boot-arg line: %s\n\n", Context->EditArgs);
    Print (L"Enter adds the arg, or removes it if it is already in the line. Esc when done.\n");
    for (Index = 0; Index < ARRAY_SIZE (Common); ++Index) {
      UnicodeSPrint (
        Lines[Index],
        sizeof (Lines[Index]),
        L"[%s] %-21s %s",
        ArgsFind (Context->EditArgs, Common[Index][0], NULL) ? L"x" : L" ",
        Common[Index][0],
        Common[Index][1]
        );
      Names[Index] = Lines[Index];
    }

    Choice = UiChoose (Names, ARRAY_SIZE (Common), (UINTN)Choice);
    if (Choice < 0) {
      return;
    }

    ArgsToggle (Context->EditArgs, Common[Choice][0]);
  }
}

/**
  Make a copy of the active config.plist in which only the text of the boot-args string
  is replaced by the edited line. Nothing else in the file is parsed or rewritten.
  Prints the reason when it cannot be done.
**/
STATIC
EFI_STATUS
BuildEditedConfig (
  IN  CONST B00T_CONTEXT  *Context,
  OUT UINT8               **NewData,
  OUT UINTN               *NewSize
  )
{
  STATIC CONST CHAR8  Key[]   = "<key>boot-args</key>";
  STATIC CONST CHAR8  Open[]  = "<string>";
  STATIC CONST CHAR8  Close[] = "</string>";
  CONST CHAR8         *Data;
  CHAR8               Old[B00T_MAX_ARGS];
  CHAR8               New[B00T_MAX_ARGS];
  UINTN               Size;
  UINTN               Index;
  UINTN               Found;
  UINTN               Position;
  UINTN               ValueStart;
  UINTN               ValueEnd;
  UINTN               OldLength;
  UINTN               NewLength;

  Data     = (CONST CHAR8 *)Context->ConfigData;
  Size     = Context->ConfigSize;
  Found    = 0;
  Position = 0;
  for (Index = 0; Index + sizeof (Key) - 1 <= Size; ++Index) {
    if (CompareMem (&Data[Index], Key, sizeof (Key) - 1) == 0) {
      ++Found;
      Position = Index + sizeof (Key) - 1;
    }
  }

  if (Found != 1) {
    Print (L"  config.plist has %u boot-args keys; this needs exactly one.\n", (UINT32)Found);
    return EFI_UNSUPPORTED;
  }

  while ((Position < Size) && ((Data[Position] == ' ') || (Data[Position] == '\t') || (Data[Position] == '\r') || (Data[Position] == '\n'))) {
    ++Position;
  }

  if ((Position + sizeof (Open) - 1 > Size) || (CompareMem (&Data[Position], Open, sizeof (Open) - 1) != 0)) {
    Print (L"  The boot-args key is not followed by a <string> value.\n");
    return EFI_UNSUPPORTED;
  }

  ValueStart = Position + sizeof (Open) - 1;
  ValueEnd   = ValueStart;
  while ((ValueEnd + sizeof (Close) - 1 <= Size) && (CompareMem (&Data[ValueEnd], Close, sizeof (Close) - 1) != 0)) {
    ++ValueEnd;
  }

  if (ValueEnd + sizeof (Close) - 1 > Size) {
    Print (L"  The boot-args <string> value has no end.\n");
    return EFI_UNSUPPORTED;
  }

  //
  // The text in the file must be exactly what OpenCore's parser reported for boot-args.
  // That rules out escaped characters and makes sure this is the string OpenCore uses.
  //
  UnicodeStrToAsciiStrS (Context->ConfigBootArgs, Old, sizeof (Old));
  UnicodeStrToAsciiStrS (Context->EditArgs, New, sizeof (New));
  OldLength = ValueEnd - ValueStart;
  NewLength = AsciiStrLen (New);
  if ((OldLength != AsciiStrLen (Old)) || (CompareMem (&Data[ValueStart], Old, OldLength) != 0)) {
    Print (L"  The boot-args text in the file is not the plain string OpenCore reads from it.\n");
    return EFI_UNSUPPORTED;
  }

  *NewSize = Size - OldLength + NewLength;
  *NewData = AllocatePool (*NewSize);
  if (*NewData == NULL) {
    Print (L"  Out of memory.\n");
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (*NewData, Data, ValueStart);
  CopyMem (*NewData + ValueStart, New, NewLength);
  CopyMem (*NewData + ValueStart + NewLength, &Data[ValueEnd], Size - ValueEnd);
  return EFI_SUCCESS;
}

/**
  COMMIT and APPLY & REBOOT: put the edited boot-arg line into a copy of config.plist,
  pre-flight the copy, and swap it in through the Phase 4 path. With Reset the caller
  cold resets; without, the tool carries on with the new config.
**/
STATIC
B00T_UI_RESULT
UiSaveArgs (
  IN OUT B00T_CONTEXT  *Context,
  IN     BOOLEAN       Reset
  )
{
  EFI_STATUS    Status;
  B00T_VARIANT  Edited;
  CHAR16        Serial[32];
  CHAR16        Args[B00T_MAX_ARGS];
  UINTN         Index;

  UiPageBegin (Reset ? L"APPLY & REBOOT" : L"COMMIT");
  if (StrCmp (Context->EditArgs, Context->ConfigBootArgs) == 0) {
    Print (L"The boot-arg line is the same as in config.plist. Nothing to save.\n");
    UiPageEnd ();
    return B00tUiStay;
  }

  Print (L"boot-args in config.plist now: %s\n", Context->ConfigBootArgs);
  Print (L"new boot-args:                 %s\n\n", Context->EditArgs);

  ZeroMem (&Edited, sizeof (Edited));
  StrCpyS (Edited.Name, ARRAY_SIZE (Edited.Name), L"edited-boot-args");
  Print (L"Edit a copy of config.plist:\n");
  Status = BuildEditedConfig (Context, &Edited.Data, &Edited.Size);
  if (EFI_ERROR (Status)) {
    UiPrintWarning (L"\nREFUSED: cannot edit this config safely. Nothing was changed.\n");
    UiPageEnd ();
    return B00tUiStay;
  }

  Print (L"  OK, %u bytes (was %u)\n", (UINT32)Edited.Size, (UINT32)Context->ConfigSize);
  Sha256 (Edited.Hash, Edited.Data, Edited.Size);

  Print (L"Pre-flight check of the copy:\n");
  if (!PreflightVariant (Context->OcDir, &Edited)) {
    UiPrintWarning (L"\nREFUSED: the edited copy fails the pre-flight. Nothing was changed.\n");
    FreePool (Edited.Data);
    UiPageEnd ();
    return B00tUiStay;
  }

  Print (L"Read the copy back with OpenCore's parser:\n");
  if (  !ParseConfigInfo (Edited.Data, Edited.Size, Serial, ARRAY_SIZE (Serial), Args, ARRAY_SIZE (Args))
     || (StrCmp (Args, Context->EditArgs) != 0)
     || (StrCmp (Serial, Context->Serial) != 0))
  {
    UiPrintWarning (L"\nREFUSED: the copy does not read back with the new boot-args. Nothing was changed.\n");
    FreePool (Edited.Data);
    UiPageEnd ();
    return B00tUiStay;
  }

  Print (L"  OK, boot-args = %s\n", Args);
  Print (L"\nWrite the copy to config.plist%s?\n", Reset ? L" and cold reset" : L" (no reset)");
  if (UiChoose (mNoYes, ARRAY_SIZE (mNoYes), 0) != 1) {
    FreePool (Edited.Data);
    return B00tUiStay;
  }

  Print (L"\n");
  Status = SwapConfig (Context->OcDir, Context->B00tDir, Context->ConfigData, Context->ConfigSize, &Edited);
  if (EFI_ERROR (Status)) {
    UiPrintWarning (L"\nSAVE FAILED. No reset.\n");
    Print (L"Status: %r\n", Status);
    FreePool (Edited.Data);
    UiPageEnd ();
    return B00tUiExit;
  }

  DEBUG ((DEBUG_INFO, "B00T: boot-args saved to config.plist, reset %d\n", Reset));
  if (Reset) {
    Print (L"\nSaved. Cold reset...\n");
    FreePool (Edited.Data);
    return B00tUiReset;
  }

  //
  // Carry on with the new config: it is what is on disk now.
  //
  if (Context->OwnedConfig != NULL) {
    FreePool (Context->OwnedConfig);
  }

  Context->OwnedConfig = Edited.Data;
  Context->ConfigData  = Edited.Data;
  Context->ConfigSize  = Edited.Size;
  StrCpyS (Context->ConfigBootArgs, ARRAY_SIZE (Context->ConfigBootArgs), Context->EditArgs);
  for (Index = 0; Index < Context->Count; ++Index) {
    Context->Variants[Index].Active = (Context->Variants[Index].Size == Edited.Size)
                                      && (CompareMem (Context->Variants[Index].Data, Edited.Data, Edited.Size) == 0);
  }

  Print (L"\nSaved. OpenCore uses it from the next boot.\n");
  UiPageEnd ();
  return B00tUiStay;
}

/**
  B00T ONCE: set NVRAM boot-args to the edited line and go back to the picker without a
  reset. config.plist is not touched.
**/
STATIC
B00T_UI_RESULT
UiBootOnce (
  IN B00T_CONTEXT  *Context
  )
{
  EFI_STATUS  Status;
  CHAR8       Args[B00T_MAX_ARGS];

  UiPageBegin (L"B00T ONCE");
  Print (L"Sets NVRAM boot-args to:\n  %s\n", Context->EditArgs);
  Print (L"and goes back to the OpenCore picker without a reset. config.plist is not changed.\n\n");
  Print (L"Set it?\n");
  if (UiChoose (mNoYes, ARRAY_SIZE (mNoYes), 0) != 1) {
    return B00tUiStay;
  }

  UnicodeStrToAsciiStrS (Context->EditArgs, Args, sizeof (Args));
  Status = gRT->SetVariable (
                  L"boot-args",
                  &gAppleBootVariableGuid,
                  EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                  AsciiStrLen (Args),
                  Args
                  );
  Print (L"\nSet NVRAM boot-args - %r\n", Status);
  if (EFI_ERROR (Status)) {
    UiPageEnd ();
    return B00tUiStay;
  }

  Print (L"Back to the picker...\n");
  gBS->Stall (1500000);
  return B00tUiExit;
}

/**
  Run the main screen until the user leaves or an action needs a cold reset.
**/
STATIC
B00T_UI_RESULT
UiLoop (
  IN OUT B00T_CONTEXT  *Context
  )
{
  EFI_INPUT_KEY   Key;
  B00T_UI_RESULT  Result;
  UINTN           Selected;
  UINTN           Next;
  UINTN           Index;
  UINTN           Yellow;
  BOOLEAN         Redraw;
  B00T_VARIANT    *KnownGood;

  ParseConfigInfo (
    Context->ConfigData,
    Context->ConfigSize,
    Context->Serial,
    ARRAY_SIZE (Context->Serial),
    Context->ConfigBootArgs,
    ARRAY_SIZE (Context->ConfigBootArgs)
    );
  StrCpyS (Context->EditArgs, ARRAY_SIZE (Context->EditArgs), Context->ConfigBootArgs);
  UiLoadInfo (Context);

  KnownGood = NULL;
  for (Index = 0; Index < Context->Count; ++Index) {
    if (B00tStriEqual (Context->Variants[Index].Name, B00T_KNOWN_GOOD)) {
      KnownGood = &Context->Variants[Index];
    }
  }

  Yellow          = EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK);
  Context->Cursor = 0;
  Context->Top    = 0;
  Selected        = Context->Count > 0 ? B00T_SEL_LIST : B00T_SEL_RESTORE;
  Redraw          = TRUE;
  while (TRUE) {
    if (Redraw) {
      UiDrawMain ();
      UiDrawInfo (Context);
      for (Index = 0; Index < B00T_SEL_COUNT; ++Index) {
        UiDrawSel (Context, Index, Index == Selected);
      }

      UiDrawArgs (Context, FALSE, Selected);
      UiDrawList (Context, Selected == B00T_SEL_LIST);
      UiDrawOverview (Context);
      UiStatus (Yellow, mUiSel[Selected].Hint);
      Redraw = FALSE;
    }

    Key = WaitForKey ();
    if (Key.ScanCode == SCAN_ESC) {
      return B00tUiExit;
    }

    if ((Key.ScanCode == SCAN_UP) || (Key.ScanCode == SCAN_DOWN) || (Key.ScanCode == SCAN_LEFT) || (Key.ScanCode == SCAN_RIGHT)) {
      //
      // Inside the variant list up and down move its cursor first.
      //
      if ((Selected == B00T_SEL_LIST) && (Key.ScanCode == SCAN_UP) && (Context->Cursor > 0)) {
        --Context->Cursor;
        Context->Top = MIN (Context->Top, Context->Cursor);
        UiDrawList (Context, TRUE);
        UiDrawOverview (Context);
        continue;
      }

      if ((Selected == B00T_SEL_LIST) && (Key.ScanCode == SCAN_DOWN) && (Context->Cursor + 1 < Context->Count)) {
        ++Context->Cursor;
        if (Context->Cursor >= Context->Top + B00T_UI_LIST_ROWS) {
          Context->Top = Context->Cursor + 1 - B00T_UI_LIST_ROWS;
        }

        UiDrawList (Context, TRUE);
        UiDrawOverview (Context);
        continue;
      }

      Next = UiNeighbour (Context, Selected, Key.ScanCode);
      if (Next == Selected) {
        continue;
      }

      UiDrawSel (Context, Selected, FALSE);
      UiDrawSel (Context, Next, TRUE);
      if ((Selected == B00T_SEL_LIST) || (Next == B00T_SEL_LIST)) {
        UiDrawList (Context, Next == B00T_SEL_LIST);
      }

      Selected = Next;
      UiStatus (Yellow, mUiSel[Selected].Hint);
      continue;
    }

    if (Key.UnicodeChar != CHAR_CARRIAGE_RETURN) {
      continue;
    }

    switch (Selected) {
      case B00T_SEL_VERBOSE:
      case B00T_SEL_SAFE:
      case B00T_SEL_SINGLE:
      case B00T_SEL_DEBUG:
        ArgsToggle (Context->EditArgs, mCheckTokens[Selected]);
        UiDrawArgs (Context, FALSE, Selected);
        continue;
      case B00T_SEL_FIELD:
        UiEditField (Context);
        UiDrawArgs (Context, FALSE, Selected);
        UiStatus (Yellow, mUiSel[Selected].Hint);
        continue;
      case B00T_SEL_ARGS_LIST:
        UiArgsList (Context);
        Result = B00tUiStay;
        break;
      case B00T_SEL_COMMIT:
        Result = UiSaveArgs (Context, FALSE);
        break;
      case B00T_SEL_APPLY:
        Result = UiSaveArgs (Context, TRUE);
        break;
      case B00T_SEL_ONCE:
        Result = UiBootOnce (Context);
        break;
      case B00T_SEL_DISCARD:
        return B00tUiExit;
      case B00T_SEL_LIST:
        Result = UiActivate (Context, &Context->Variants[Context->Cursor]);
        break;
      case B00T_SEL_RESTORE:
        Result = UiActivate (Context, KnownGood);
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
B00T_UI_RESULT
UiRun (
  IN OUT B00T_CONTEXT  *Context
  )
{
  B00T_UI_RESULT  Result;

  Result = UiLoop (Context);
  if (Context->OwnedConfig != NULL) {
    //
    // The caller still owns the buffer it passed in; only the COMMIT copy is freed here.
    //
    FreePool (Context->OwnedConfig);
    Context->OwnedConfig = NULL;
  }

  return Result;
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
