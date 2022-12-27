/** @file
  Provide usable GOP on unsupported graphics cards on classic MacPro.

  Copyright (c) 2022, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <PiDxe.h>
#include <Uefi.h>

#include <Guid/AppleFile.h>
#include <Guid/AppleVariable.h>

#include <Library/DuetBdsLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcConsoleLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/OcDeviceMiscLib.h>
#include <Library/OcMiscLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/UgaDraw.h>

#include <Protocol/FirmwareVolume.h>
#include <Protocol/FirmwareVolume2.h>

#define __UEFI_MULTIPHASE_H__
#define __PI_MULTIPHASE_H__
#include <Pi/PiDxeCis.h>

#define EFI_BLUETOOTH_DELAY_DEFAULT 3000

STATIC EFI_DISPATCH mOriginalDispatch;

STATIC UINTN                          NumOfFvHandles;
STATIC UINTN                          NumOfFv2Handles;
STATIC EFI_FIRMWARE_VOLUME_PROTOCOL   **FvInterfaces;
STATIC EFI_FIRMWARE_VOLUME2_PROTOCOL  **Fv2Interfaces;
STATIC EFI_FV_READ_FILE               *Fv2ReadFiles;
STATIC FRAMEWORK_EFI_FV_READ_FILE     *FvReadFiles;
STATIC BOOLEAN                        mPrepareForUIApp = FALSE;
STATIC BOOLEAN                        mAlreadyPreparedForUIApp = FALSE;

STATIC
BOOLEAN
HasValidGop (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  Status      = gBS->HandleProtocol (
                       gST->ConsoleOutHandle,
                       &gEfiGraphicsOutputProtocolGuid,
                       (VOID **)&Gop
                       );

  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  if (Gop->Mode->MaxMode == 0) {
    return FALSE;
  }

  return TRUE;
}

STATIC
VOID
CheckForUIApp (
  IN CONST  EFI_GUID                      *NameGuid
  )
{
  BOOLEAN IsPicker;
  BOOLEAN IsPassword;

  IsPicker = CompareGuid (&gAppleBootPickerFileGuid, NameGuid);
  IsPassword = IsPicker ? FALSE : CompareGuid (&gApplePasswordUIFileGuid, NameGuid);
  if (IsPicker || IsPassword) {
    if (HasValidGop ()) {
      return;
    }

    mPrepareForUIApp = TRUE;
    if (IsPassword) {
      //
      // Picker does this always.
      // TODO: Improve security by working out exactly what needs to be connected here.
      //
      BdsLibConnectAllDriversToAllControllers ();
    }
  }
}

STATIC
EFI_STATUS
WrappedFvReadFile (
  IN EFI_FIRMWARE_VOLUME_PROTOCOL   *This,
  IN EFI_GUID                       *NameGuid,
  IN OUT VOID                       **Buffer,
  IN OUT UINTN                      *BufferSize,
  OUT EFI_FV_FILETYPE               *FoundType,
  OUT EFI_FV_FILE_ATTRIBUTES        *FileAttributes,
  OUT UINT32                        *AuthenticationStatus
  )
{
  UINTN Index;

  CheckForUIApp (NameGuid);

  //
  // Call the correct original ReadFile for the interface.
  //
  for (Index = 0; Index < NumOfFvHandles; Index++) {
    if (FvInterfaces[Index] == This) {
      return FvReadFiles[Index] (
        This,
        NameGuid,
        Buffer,
        BufferSize,
        FoundType,
        FileAttributes,
        AuthenticationStatus
      );
    }
  }

  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
WrappedFv2ReadFile (
  IN CONST  EFI_FIRMWARE_VOLUME2_PROTOCOL *This,
  IN CONST  EFI_GUID                      *NameGuid,
  IN OUT    VOID                          **Buffer,
  IN OUT    UINTN                         *BufferSize,
  OUT       EFI_FV_FILETYPE               *FoundType,
  OUT       EFI_FV_FILE_ATTRIBUTES        *FileAttributes,
  OUT       UINT32                        *AuthenticationStatus
  )
{
  UINTN Index;

  CheckForUIApp (NameGuid);

  //
  // Call the correct original ReadFile for the interface.
  //
  for (Index = 0; Index < NumOfFv2Handles; Index++) {
    if (Fv2Interfaces[Index] == This) {
      return Fv2ReadFiles[Index] (
        This,
        NameGuid,
        Buffer,
        BufferSize,
        FoundType,
        FileAttributes,
        AuthenticationStatus
      );
    }
  }

  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
WrapFvReadFile (
  IN BOOLEAN                     UseFv2
  )
{
  UINTN                          Index;
  EFI_STATUS                     Status;
  EFI_STATUS                     TempStatus;
  EFI_HANDLE                     CurrentVolume;
  UINTN                          NumOfHandles;
  EFI_HANDLE                     *HandleBuffer;

  if (UseFv2) {
    Status = gBS->LocateHandleBuffer (
                    ByProtocol,
                    &gEfiFirmwareVolume2ProtocolGuid,
                    NULL,
                    &NumOfFv2Handles,
                    &HandleBuffer
                    );
    NumOfHandles = NumOfFv2Handles;
  } else {
    Status = gBS->LocateHandleBuffer (
                    ByProtocol,
                    &gEfiFirmwareVolumeProtocolGuid,
                    NULL,
                    &NumOfFvHandles,
                    &HandleBuffer
                    );
    NumOfHandles = NumOfFvHandles;
  }

  if (EFI_ERROR (Status)) {
    if (UseFv2) {
      NumOfFv2Handles = 0;
    } else {
      NumOfFvHandles = 0;
    }
    return Status;
  }

  if (UseFv2) {
    Fv2ReadFiles = AllocatePool (sizeof (*Fv2ReadFiles) * NumOfHandles);
    if (Fv2ReadFiles == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    Fv2Interfaces = AllocatePool (sizeof (*Fv2Interfaces) * NumOfHandles);
    if (Fv2Interfaces == NULL) {
      FreePool (Fv2ReadFiles);
      return EFI_OUT_OF_RESOURCES;
    }
  } else {
    FvReadFiles = AllocatePool (sizeof (*FvReadFiles) * NumOfHandles);
    if (FvReadFiles == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    FvInterfaces = AllocatePool (sizeof (*FvInterfaces) * NumOfHandles);
    if (FvInterfaces == NULL) {
      FreePool (FvReadFiles);
      return EFI_OUT_OF_RESOURCES;
    }
  }

  for (Index = 0; Index < NumOfHandles; ++Index) {
    CurrentVolume = HandleBuffer[Index];

    if (UseFv2) {
      TempStatus = gBS->HandleProtocol (
                      CurrentVolume,
                      &gEfiFirmwareVolume2ProtocolGuid,
                      (VOID **)&Fv2Interfaces[Index]
                      );
    } else {
      TempStatus = gBS->HandleProtocol (
                      CurrentVolume,
                      &gEfiFirmwareVolumeProtocolGuid,
                      (VOID **)&FvInterfaces[Index]
                      );
    }

    if (EFI_ERROR (TempStatus)) {
      Status = TempStatus;
      continue;
    }

    if (UseFv2) {
      Fv2ReadFiles[Index] = Fv2Interfaces[Index]->ReadFile;
      Fv2Interfaces[Index]->ReadFile = WrappedFv2ReadFile;
    } else {
      FvReadFiles[Index] = FvInterfaces[Index]->ReadFile;
      FvInterfaces[Index]->ReadFile = WrappedFvReadFile;
    }
  }

  FreePool (HandleBuffer);
  return Status;
}

//
// Provide searchable string in compiled binary.
//
STATIC CHAR8 UseDirectGop[] = "DirectGopRendering=0";

STATIC
VOID
OcLoadUefiOutputSupport (
  VOID
  )
{
  OcProvideConsoleGop (FALSE);

  OcSetConsoleResolution (
              0,
              0,
              0,
              FALSE
              );

  if (UseDirectGop[L_STR_LEN (UseDirectGop) - 1] == '1') {
    OcUseDirectGop (-1);
  }

  OcSetupConsole (
    OcConsoleRendererBuiltinGraphics,
    FALSE,
    FALSE,
    FALSE,
    FALSE
    );
}

STATIC
EFI_STATUS
EFIAPI
WrappedDispatch (
  VOID
  )
{
  EFI_STATUS        Status;
  STATIC BOOLEAN    Nested = FALSE;

  if (Nested) {
    return mOriginalDispatch ();
  }

  Nested = TRUE;
  Status = mOriginalDispatch ();
  if (mPrepareForUIApp && !mAlreadyPreparedForUIApp && Status == EFI_NOT_FOUND) {
    //
    // Executes at end of driver binding via BdsLibConnectAllDriversToAllControllers algorithm.
    //
    OcLoadUefiOutputSupport ();
    OcUnlockAppleBootPicker ();

    mAlreadyPreparedForUIApp = TRUE;
  }
  Nested = FALSE;
  
  return Status;
}

STATIC
VOID
WrapDispatch (
  VOID
  )
{
  mOriginalDispatch = gDS->Dispatch;
  gDS->Dispatch = WrappedDispatch;
}

STATIC
EFI_STATUS
SetDefaultVariable (
  CHAR16    *Name,
  EFI_GUID  *Guid,
  UINTN     Size,
  VOID      *Data
  )
{
  EFI_STATUS  Status;
  UINTN       DataSize;

  DataSize = 0;
  Status = gRT->GetVariable (
    Name,
    Guid,
    NULL,
    &DataSize,
    NULL
  );

  if (Status == EFI_BUFFER_TOO_SMALL) {
    return EFI_SUCCESS;
  }

  Status = gRT->SetVariable (
    Name,
    Guid,
    EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
    Size,
    Data
  );

  return Status;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT16 EFIBluetoothDelay;

  //
  // If not present and large enough, Apple picker is never entered on some GPUs.
  // Gets (re-)set on first boot of macOS.
  //
  EFIBluetoothDelay = EFI_BLUETOOTH_DELAY_DEFAULT;
  SetDefaultVariable (
    L"EFIBluetoothDelay",
    &gAppleBootVariableGuid,
    sizeof (EFIBluetoothDelay),
    &EFIBluetoothDelay
  );

  OcForgeUefiSupport (TRUE, TRUE);
  OcReloadOptionRoms ();

  WrapFvReadFile (FALSE);
  WrapFvReadFile (TRUE);

  WrapDispatch ();
 
  return EFI_SUCCESS;
}
