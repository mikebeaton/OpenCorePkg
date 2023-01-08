/** @file
  Provide GOP on unsupported graphics cards on classic MacPro.

  Copyright (c) 2022-2023, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DuetBdsLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcConsoleLib.h>
#include <Library/OcDeviceMiscLib.h>
#include <Library/OcDirectResetLib.h>

#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Library/OcVariableLib.h>

#include <Protocol/Bds.h>

#define EFI_BLUETOOTH_DELAY_DEFAULT  3000

STATIC EFI_GET_MEMORY_SPACE_MAP mOriginalGetMemorySpaceMap;
STATIC EFI_CALCULATE_CRC32 mOriginalCalculateCrc32;
STATIC EFI_BDS_ENTRY mOriginalBdsEntry;
STATIC EFI_INSTALL_PROTOCOL_INTERFACE mOriginalInstallProtocolInterface;

STATIC UINTN mgSTCount = 0;
STATIC UINTN mCrcCount = 0;

// #define VBIOS_TAG

STATIC
VOID
LogStage (
  CHAR8       Stage,
  EFI_STATUS  Status
  )
{
#ifdef VBIOS_TAG
  CHAR16  Name[] = L"efi-gop-xbios0";
#else
  CHAR16  Name[] = L"efi-gop-stage0";
#endif

  // DEBUG_CODE_BEGIN ();

  if (FeaturePcdGet (PcdEnableGopDirect)) {
    Name[6] = L'd';
  }

  Name[L_STR_LEN (Name) - 1] = Stage;
  OcSetSystemVariable (
    Name,
    OPEN_CORE_NVRAM_ATTR,
    sizeof (Status),
    &Status,
    NULL
    );

  // DEBUG_CODE_END ();
}

STATIC
VOID
LogSystemTable (
  CHAR16      *TableName,
  VOID        *Table,
  CHAR8       Stage
  )
{
  CHAR16  Name[] = L"001-gop-xxx-hdr-0";

  // DEBUG_CODE_BEGIN ();

  if (FeaturePcdGet (PcdEnableGopDirect)) {
    Name[6] = L'd';
  }

  CopyMem (Name + 8, TableName, 3 * sizeof(CHAR16));
  Name[L_STR_LEN (Name) - 1] = Stage;
  OcSetSystemVariable (
    Name,
    OPEN_CORE_NVRAM_ATTR,
    sizeof (gST->Hdr),
    Table,
    NULL
    );

  // DEBUG_CODE_END ();
}

STATIC
VOID
Log_gST (
  CHAR8       Stage
  )
{
  CHAR16 *FirmwareVendor;
  CHAR16  Name2[] = L"gST-FirmwareRevision-0";
  CHAR16  Buffer[20];

  FirmwareVendor = gST->FirmwareVendor;
  if (FirmwareVendor == NULL) {
    FirmwareVendor = L"(null)";
  }
  // DEBUG_CODE_BEGIN ();

  StrnCpyS (Buffer, 20, FirmwareVendor, StrLen(FirmwareVendor));
  Buffer[0] = Stage;
  OcSetSystemVariable (
    Buffer,
    OPEN_CORE_NVRAM_ATTR,
    sizeof (Stage),
    &Stage,
    NULL
    );

  Name2[L_STR_LEN (Name2) - 1] = Stage;
  OcSetSystemVariable (
    Name2,
    OPEN_CORE_NVRAM_ATTR,
    sizeof (gST->FirmwareRevision),
    &gST->FirmwareRevision,
    NULL
    );

  // DEBUG_CODE_END ();
}

STATIC
VOID
LogSystemTables (
  CHAR8 Stage
  )
{
  Log_gST(Stage);
  LogSystemTable (L"gST", gST, Stage);
  LogSystemTable (L"gBS", gBS, Stage);
  LogSystemTable (L"gRT", gRT, Stage);
  LogSystemTable (L"gDS", gDS, Stage);
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
    LogStage ('4', Status);
    return Status;
  }

  LogStage ('5', EFI_SUCCESS);

  OcSetConsoleResolution (
    0,
    0,
    0,
    FALSE
    );

  if (FeaturePcdGet (PcdEnableGopDirect)) {
    LogStage ('6', EFI_SUCCESS);
    OcUseDirectGop (-1);
  }

#if 1
  OcSetupConsole (
    OcConsoleRendererBuiltinGraphics,
    FALSE,
    FALSE,
    FALSE,
    FALSE
    );
#else
  //
  // Trying to make additional pre-boot APFS lines go away, but this does not do it. :-(
  //
  OcSetupConsole (
    OcConsoleRendererSystemGraphics,
    TRUE,
    FALSE,
    FALSE,
    FALSE
    );
#endif

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ProvideGop (
  IN BOOLEAN  Forge
  )
{
  LogStage ('3', EFI_SUCCESS);

  OcUnlockAppleFirmwareUI ();

  if (Forge) {
    //
    // When we forge late, basically as if loaded at Driver#### time,
    // we have to reload everything.
    //
    OcForgeUefiSupport (TRUE, TRUE);
    OcReloadOptionRoms ();

    //
    // Use the same algorithm which is present in Apple picker.
    //
    BdsLibConnectAllDriversToAllControllers ();
  }

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
// This memory map access happens in the equivalent of efi InitializeMemoryTest,
// at the start of PlatformBdsPolicyBehavior which (is just after BdsLibLoadDrivers,
// in versions where it exists; e.g. MacPro 144.0.0.0.0, but not some others) in
// Apple EFI 1.1 firmware. (This is also shortly after PlatformBdsInit, which
// we are also hooking into via the gBS->CRC call.)
//
EFI_STATUS
EFIAPI
WrappedGetMemorySpaceMap (
  OUT UINTN                            *NumberOfDescriptors,
  OUT EFI_GCD_MEMORY_SPACE_DESCRIPTOR  **MemorySpaceMap
  )
{
  STATIC UINTN  mGetMemorySpaceMapAccessCount = 0;

  LogStage ('t' + mGetMemorySpaceMapAccessCount, mgSTCount);
  LogStage ('k' + mGetMemorySpaceMapAccessCount, mCrcCount);

  if (mGetMemorySpaceMapAccessCount++ == 1) {
    SetBluetoothDelay ();
    ProvideGop (FALSE);
  }

  return mOriginalGetMemorySpaceMap (
           NumberOfDescriptors,
           MemorySpaceMap
           );
}

STATIC
VOID
WrapGetMemorySpaceMap (
  VOID
  )
{
  mOriginalGetMemorySpaceMap = gDS->GetMemorySpaceMap;
  gDS->GetMemorySpaceMap     = WrappedGetMemorySpaceMap;
}

EFI_STATUS
EFIAPI 
WrappedCalculateCrc32 (
  IN  VOID                              *Data,
  IN  UINTN                             DataSize,
  OUT UINT32                            *Crc32
  )
{
  EFI_STATUS  Status;
  STATIC UINTN Count = 0;

  if (Data == gST) {
    mgSTCount++;
  }
  mCrcCount++;

  if (Count < 3) {
    LogSystemTables ('0' + Count);
  }

  Status = mOriginalCalculateCrc32 (
    Data,
    DataSize,
    Crc32
  );

  if (Count < 3) {
    LogSystemTables ('5' + Count );
  }
  Count++;

  return Status;
}

STATIC
VOID
WrapCalculateCrc32 (
  VOID
  )
{
  mOriginalCalculateCrc32 = gBS->CalculateCrc32;
  gBS->CalculateCrc32 = WrappedCalculateCrc32;
}

STATIC
VOID
EFIAPI
WrappedBdsEntry (
  IN EFI_BDS_ARCH_PROTOCOL  *This
  )
{
  CHAR16 Name[] = L"Mike000";
  STATIC UINTN BdsEntryCount = 0;

  if (BdsEntryCount == 0) {
    WrapCalculateCrc32 ();
    // WrapGetMemorySpaceMap ();
  }

  Name[L_STR_LEN (Name) - 1 - BdsEntryCount] = '1';

  gST->FirmwareVendor = Name;

  BdsEntryCount++;

  return mOriginalBdsEntry (This);
}

STATIC
VOID
WrapBdsEntry (
  IN OUT EFI_BDS_ARCH_PROTOCOL     *Bds
  )
{
  mOriginalBdsEntry = Bds->Entry;
  Bds->Entry = WrappedBdsEntry;
}

STATIC
EFI_STATUS
EFIAPI
WrappedInstallProtocolInterface (
  IN OUT EFI_HANDLE               *Handle,
  IN     EFI_GUID                 *Protocol,
  IN     EFI_INTERFACE_TYPE       InterfaceType,
  IN     VOID                     *Interface
  )
{
  STATIC BOOLEAN Triggered = FALSE;

  if (!Triggered && CompareGuid (&gEfiBdsArchProtocolGuid, Protocol)) {
    Triggered = TRUE;
    WrapBdsEntry (Interface);
  }

  return mOriginalInstallProtocolInterface (
    Handle,
    Protocol,
    InterfaceType,
    Interface
  );
}

// STATIC
VOID
WrapInstallProtocolInterface (
  VOID
  )
{
  mOriginalInstallProtocolInterface = gBS->InstallProtocolInterface;
  gBS->InstallProtocolInterface = WrappedInstallProtocolInterface;
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
#ifdef VBIOS_TAG
  OcVariableInit (FALSE);
  LogStage ('0', EFI_SUCCESS);
  LogStage ('2', EFI_SUCCESS);
  return EFI_SUCCESS;
#else
  EFI_STATUS  Status;

  // DEBUG_CODE_BEGIN ();
  OcVariableInit (FALSE);
  // DEBUG_CODE_END ();

  Status = SetBluetoothDelay ();

  LogStage ('X', Status);

  //
  // Are we loaded as driver or injected?
  // EFI_END_OF_MEDIA is returned when the driver is injected anywhere
  // in the Apple system file volume without dependencies.
  //
  // // if (EFI_ERROR (Status)) {
    OcForgeUefiSupport (TRUE, TRUE);
    WrapGetMemorySpaceMap ();
    WrapInstallProtocolInterface ();
  // // } else {
  // //   ProvideGop (TRUE);
  // // }

  return EFI_SUCCESS;
#endif
}
