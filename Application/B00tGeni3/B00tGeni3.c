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

  Every write goes to a temporary name first and is read back before it is
  renamed into place, and there is always a complete config on disk
  (config.plist or config.old).

  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcCryptoLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define B00T_VERSION         L"0.4"
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
RenameFile (
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

  //
  // Drop an existing target first; FAT will not rename over it.
  //
  Status = DeleteIfExists (Dir, NewName);
  if (EFI_ERROR (Status)) {
    return Status;
  }

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
    Status = gBS->HandleProtocol (LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&FileSystem);
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

  while (TRUE) {
    ShowMenu (ConfigData, ConfigSize, ActiveName[0] != '\0' ? ActiveName : NULL, Variants, Count);
    Key = WaitForKey ();
    if ((Key.ScanCode == SCAN_ESC) || (Key.UnicodeChar == L'q') || (Key.UnicodeChar == L'Q')) {
      break;
    }

    if ((Key.UnicodeChar < L'1') || (Key.UnicodeChar > L'0' + Count)) {
      continue;
    }

    Choice = Key.UnicodeChar - L'1';
    if (Variants[Choice].Active) {
      Print (L"\n%s is already active.\n", Variants[Choice].Name);
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
