/** @file
  Provide GOP on unsupported graphics cards on classic MacPro.

  Copyright (c) 2022-2023, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DuetBdsLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcConsoleLib.h>
#include <Library/OcDeviceMiscLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Library/OcVariableLib.h>

#define EFI_BLUETOOTH_DELAY_DEFAULT  3000

STATIC EFI_GET_MEMORY_SPACE_MAP  mOriginalGetMemorySpaceMap = NULL;

STATIC
VOID
LogStage (
  CHAR8  Stage
  )
{
  UINT8   One    = 1;
  CHAR16  Name[] = L"efi-gop-stage0";

  DEBUG_CODE_BEGIN ();

  if (FeaturePcdGet (PcdEnableGopDirect)) {
    Name[6] = L'd';
  }

  Name[L_STR_LEN (Name) - 1] = Stage;
  OcSetSystemVariable (
    Name,
    OPEN_CORE_NVRAM_ATTR,
    sizeof (One),
    &One,
    NULL
    );

  DEBUG_CODE_END ();
}

STATIC
EFI_STATUS
LoadUefiOutputSupport (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = OcProvideConsoleGop (FALSE);
  if (EFI_ERROR (Status)) {
    LogStage ('2');
    return Status;
  }

  LogStage ('3');

  OcSetConsoleResolution (
    0,
    0,
    0,
    FALSE
    );

  if (FeaturePcdGet (PcdEnableGopDirect)) {
    LogStage ('4');
    OcUseDirectGop (-1);
  }

  OcSetupConsole (
    OcConsoleRendererBuiltinGraphics,
    FALSE,
    FALSE,
    FALSE,
    FALSE
    );

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ProvideGop (
  VOID
  )
{
  LogStage ('1');

  OcResetAppleFirmwareUIConnectGop ();

  OcForgeUefiSupport (TRUE, TRUE);
  OcReloadOptionRoms ();

  BdsLibConnectAllDriversToAllControllers ();

  return LoadUefiOutputSupport ();
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
  Status   = gRT->GetVariable (
                    Name,
                    Guid,
                    NULL,
                    &DataSize,
                    NULL
                    );

  //
  // Do not modify existing value.
  //
  if (Status == EFI_BUFFER_TOO_SMALL) {
    return EFI_SUCCESS;
  }

  return gRT->SetVariable (
                Name,
                Guid,
                EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                Size,
                Data
                );
}

//
// If not present and large enough, Apple picker is never entered on some GPUs.
// Gets (re-)set on first boot of macOS.
//
STATIC
EFI_STATUS
SetBluetoothDelay (
  VOID
  )
{
  UINT16  EFIBluetoothDelay;

  EFIBluetoothDelay = EFI_BLUETOOTH_DELAY_DEFAULT;

  return SetDefaultVariable (
           L"EFIBluetoothDelay",
           &gAppleBootVariableGuid,
           sizeof (EFIBluetoothDelay),
           &EFIBluetoothDelay
           );
}

//
// This memory map access happens in the equivalent of efi InitializeMemoryTest
// at the start of PlatformBdsPolicyBehavior and just after BdsLibLoadDrivers
// in 144.0.0.0.0 Mac Pro firmware.
//
EFI_STATUS
EFIAPI
WrappedGetMemorySpaceMap (
  OUT UINTN                            *NumberOfDescriptors,
  OUT EFI_GCD_MEMORY_SPACE_DESCRIPTOR  **MemorySpaceMap
  )
{
  STATIC UINTN  mGetMemorySpaceMapAccessCount = 0;

  mGetMemorySpaceMapAccessCount++;

  if (mGetMemorySpaceMapAccessCount == 1) {
    SetBluetoothDelay ();
    ProvideGop ();
  }

  return mOriginalGetMemorySpaceMap (
           NumberOfDescriptors,
           MemorySpaceMap
           );
}

STATIC
EFI_STATUS
WrapGetMemorySpaceMap (
  VOID
  )
{
  mOriginalGetMemorySpaceMap = gDS->GetMemorySpaceMap;
  gDS->GetMemorySpaceMap     = WrappedGetMemorySpaceMap;

  gDS->Hdr.CRC32 = 0;
  gDS->Hdr.CRC32 = CalculateCrc32 (gDS, gDS->Hdr.HeaderSize);

  return EFI_SUCCESS;
}

//
// If driver is injected with all dependencies from OC UefiDriverEntryPoint.inf
// manually specified, then it loads late enough to provide GOP, but still too
// early (we get picker with no entries), so strategy is to wrap a call which
// happens at similar time to normal Driver#### load.
//
EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  DEBUG_CODE_BEGIN ();
  OcVariableInit (FALSE);
  DEBUG_CODE_END ();

  LogStage ('X');

  Status = SetBluetoothDelay ();

  //
  // Are we loaded as driver or injected?
  // EFI_END_OF_MEDIA is returned when the driver is injected anywhere
  // in the Apple system file volume without dependencies.
  //
  if (EFI_ERROR (Status)) {
    Status = WrapGetMemorySpaceMap ();
  } else {
    Status = ProvideGop ();
  }

  return Status;
}
