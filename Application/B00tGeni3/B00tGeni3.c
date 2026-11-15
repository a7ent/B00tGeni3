/** @file
  B00tGeni3 config picker tool.

  Phase 2 (hello tool): print the device path and file path this image was
  loaded from.

  Phase 3 (write and reboot proof): write a known test file into
  EFI\OC\B00tGeni3 on the volume this image was loaded from, via a temporary
  name and a rename, read it back and compare, then cold reset.

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
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define B00T_DIR_PATH        L"EFI\\OC\\B00tGeni3"
#define B00T_TEST_TMP_NAME   L"test.tmp"
#define B00T_TEST_NAME       L"test.bin"
#define B00T_TEST_SIZE       4096
#define B00T_TEST_HEADER     "B00tGeni3 Phase 3 write test v1\n"

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
    DEBUG ((DEBUG_INFO, "B00T: %s: <none>\n", Label));
    return;
  }

  Text = ConvertDevicePathToText (DevicePath, FALSE, FALSE);
  if (Text == NULL) {
    Print (L"%s: <cannot convert>\n", Label);
    DEBUG ((DEBUG_INFO, "B00T: %s: <cannot convert>\n", Label));
    return;
  }

  Print (L"%s: %s\n", Label, Text);
  DEBUG ((DEBUG_INFO, "B00T: %s: %s\n", Label, Text));
  FreePool (Text);
}

STATIC
VOID
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

  gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
  gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
}

/**
  Fill Buffer with the Phase 3 test pattern: an ASCII header followed by
  bytes (Index * 31 + 7) & 0xFF. The host recomputes this to check the hash.
**/
STATIC
VOID
FillTestPattern (
  OUT UINT8  *Buffer
  )
{
  UINTN  Index;
  UINTN  HeaderSize;

  HeaderSize = AsciiStrLen (B00T_TEST_HEADER);
  CopyMem (Buffer, B00T_TEST_HEADER, HeaderSize);
  for (Index = HeaderSize; Index < B00T_TEST_SIZE; ++Index) {
    Buffer[Index] = (UINT8)((Index * 31 + 7) & 0xFF);
  }
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
  Status = Dir->Open (Dir, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status)) {
    Status = File->Delete (File);
    if (EFI_ERROR (Status)) {
      return Status;
    }
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
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINT8              *Buffer;
  UINTN              ReadSize;

  Buffer = AllocatePool (Size + 1);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = Dir->Open (Dir, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    FreePool (Buffer);
    return Status;
  }

  //
  // Ask for one byte more than expected so a longer file is caught too.
  //
  ReadSize = Size + 1;
  Status   = File->Read (File, &ReadSize, Buffer);
  File->Close (File);

  if (!EFI_ERROR (Status) && ((ReadSize != Size) || (CompareMem (Buffer, Expected, Size) != 0))) {
    Status = EFI_CRC_ERROR;
  }

  FreePool (Buffer);
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
  Status = Dir->Open (Dir, &File, (CHAR16 *)NewName, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status)) {
    Status = File->Delete (File);
    if (EFI_ERROR (Status)) {
      return Status;
    }
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

STATIC
EFI_STATUS
RunWriteTest (
  IN EFI_HANDLE  DeviceHandle
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *Dir;
  UINT8                            *Pattern;

  Status = gBS->HandleProtocol (DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&FileSystem);
  if (EFI_ERROR (Status)) {
    Print (L"No file system on own device - %r\n", Status);
    return Status;
  }

  Status = FileSystem->OpenVolume (FileSystem, &Root);
  if (EFI_ERROR (Status)) {
    Print (L"Cannot open volume - %r\n", Status);
    return Status;
  }

  Status = Root->Open (
                   Root,
                   &Dir,
                   B00T_DIR_PATH,
                   EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                   EFI_FILE_DIRECTORY
                   );
  if (EFI_ERROR (Status)) {
    Print (L"Cannot open or create %s - %r\n", B00T_DIR_PATH, Status);
    Root->Close (Root);
    return Status;
  }

  Pattern = AllocatePool (B00T_TEST_SIZE);
  if (Pattern == NULL) {
    Dir->Close (Dir);
    Root->Close (Root);
    return EFI_OUT_OF_RESOURCES;
  }

  FillTestPattern (Pattern);

  Status = WriteWholeFile (Dir, B00T_TEST_TMP_NAME, Pattern, B00T_TEST_SIZE);
  Print (L"Write  %s\\%s (%u bytes) - %r\n", B00T_DIR_PATH, B00T_TEST_TMP_NAME, B00T_TEST_SIZE, Status);

  if (!EFI_ERROR (Status)) {
    Status = ReadAndCompare (Dir, B00T_TEST_TMP_NAME, Pattern, B00T_TEST_SIZE);
    Print (L"Verify %s - %r\n", B00T_TEST_TMP_NAME, Status);
  }

  if (!EFI_ERROR (Status)) {
    Status = RenameFile (Dir, B00T_TEST_TMP_NAME, B00T_TEST_NAME);
    Print (L"Rename %s -> %s - %r\n", B00T_TEST_TMP_NAME, B00T_TEST_NAME, Status);
  }

  if (!EFI_ERROR (Status)) {
    Status = ReadAndCompare (Dir, B00T_TEST_NAME, Pattern, B00T_TEST_SIZE);
    Print (L"Verify %s - %r\n", B00T_TEST_NAME, Status);
  }

  DEBUG ((DEBUG_INFO, "B00T: Write test result - %r\n", Status));

  FreePool (Pattern);
  Dir->Flush (Dir);
  Dir->Close (Dir);
  Root->Flush (Root);
  Root->Close (Root);
  return Status;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL   *LoadedImageDevicePath;

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"B00tGeni3 - write and reboot test (Phase 3)\n\n");
  DEBUG ((DEBUG_INFO, "B00T: Starting B00tGeni3 write test\n"));

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    Print (L"Cannot get LoadedImage protocol - %r\n", Status);
    DEBUG ((DEBUG_WARN, "B00T: Cannot get LoadedImage protocol - %r\n", Status));
    Print (L"\nPress any key to return to OpenCore...\n");
    WaitForKey ();
    return Status;
  }

  PrintDevicePath (L"Device", DevicePathFromHandle (LoadedImage->DeviceHandle));
  PrintDevicePath (L"File  ", LoadedImage->FilePath);

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageDevicePathProtocolGuid,
                  (VOID **)&LoadedImageDevicePath
                  );
  if (!EFI_ERROR (Status)) {
    PrintDevicePath (L"Full  ", LoadedImageDevicePath);
  }

  Print (L"\n");
  Status = RunWriteTest (LoadedImage->DeviceHandle);

  if (EFI_ERROR (Status)) {
    Print (L"\nWRITE TEST FAILED - %r. No reset.\nPress any key to return to OpenCore...\n", Status);
    WaitForKey ();
    return Status;
  }

  Print (L"\nWRITE TEST PASSED.\nPress any key to cold reset...\n");
  WaitForKey ();
  DEBUG ((DEBUG_INFO, "B00T: Cold reset\n"));
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);

  //
  // Not reached unless the reset fails.
  //
  Print (L"Cold reset returned. Press any key to return to OpenCore...\n");
  WaitForKey ();
  return EFI_DEVICE_ERROR;
}
