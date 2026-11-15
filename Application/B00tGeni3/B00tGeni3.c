/** @file
  B00tGeni3 config picker tool.

  Phase 2 (hello tool): print the device path and file path this image was
  loaded from, wait for a key, and return to the OpenCore picker.

  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

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
  Print (L"B00tGeni3 - hello tool (Phase 2)\n\n");
  DEBUG ((DEBUG_INFO, "B00T: Starting B00tGeni3 hello tool\n"));

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    Print (L"Cannot get LoadedImage protocol - %r\n", Status);
    DEBUG ((DEBUG_WARN, "B00T: Cannot get LoadedImage protocol - %r\n", Status));
  } else {
    PrintDevicePath (L"Device", DevicePathFromHandle (LoadedImage->DeviceHandle));
    PrintDevicePath (L"File  ", LoadedImage->FilePath);
  }

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageDevicePathProtocolGuid,
                  (VOID **)&LoadedImageDevicePath
                  );
  if (EFI_ERROR (Status)) {
    Print (L"Full  : <no LoadedImageDevicePath - %r>\n", Status);
    DEBUG ((DEBUG_INFO, "B00T: No LoadedImageDevicePath - %r\n", Status));
  } else {
    PrintDevicePath (L"Full  ", LoadedImageDevicePath);
  }

  Print (L"\nPress any key to return to OpenCore...\n");
  WaitForKey ();
  DEBUG ((DEBUG_INFO, "B00T: Key pressed, returning to OpenCore\n"));

  return EFI_SUCCESS;
}
