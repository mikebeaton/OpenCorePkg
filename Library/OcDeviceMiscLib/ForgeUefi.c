/** @file
  Copyright (c) 2020, joevt. All rights reserved.
  Copyright (C) 2021, vit9696. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcDeviceMiscLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define __UEFI_MULTIPHASE_H__
#define __PI_MULTIPHASE_H__
#include <Pi/PiDxeCis.h>

STATIC
EFI_STATUS
EFIAPI
OcCreateEventEx (
  IN       UINT32            Type,
  IN       EFI_TPL           NotifyTpl,
  IN       EFI_EVENT_NOTIFY  NotifyFunction OPTIONAL,
  IN CONST VOID              *NotifyContext OPTIONAL,
  IN CONST EFI_GUID          *EventGroup    OPTIONAL,
  OUT      EFI_EVENT         *Event
  )
{
  if ((Type == EVT_NOTIFY_SIGNAL) && CompareGuid (EventGroup, &gEfiEventExitBootServicesGuid)) {
    return gBS->CreateEvent (
                  EVT_SIGNAL_EXIT_BOOT_SERVICES,
                  NotifyTpl,
                  NotifyFunction,
                  (VOID *)NotifyContext,
                  Event
                  );
  }

  if ((Type == EVT_NOTIFY_SIGNAL) && CompareGuid (EventGroup, &gEfiEventVirtualAddressChangeGuid)) {
    return gBS->CreateEvent (
                  EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE,
                  NotifyTpl,
                  NotifyFunction,
                  (VOID *)NotifyContext,
                  Event
                  );
  }

  gBS->CreateEvent (
         Type,
         NotifyTpl,
         NotifyFunction,
         (VOID *)NotifyContext,
         Event
         );
  return EFI_SUCCESS;
}

EFI_STATUS
OcForgeUefiSupport (
  IN BOOLEAN                        Forge,
  IN BOOLEAN                        Trash
  )
{
  EFI_BOOT_SERVICES  *NewBS;
  UINT64             Signature;

  DEBUG ((
    DEBUG_INFO,
    "OCDM: Found 0x%X/0x%X UEFI version (%u bytes, %u %a to %u) gST %p gBS %p gBS->CreateEventEx %p &gBS %p\n",
    gST->Hdr.Revision,
    gBS->Hdr.Revision,
    gBS->Hdr.HeaderSize,
    Forge,
    Trash ? "trashing" : "rebuilding",
    (UINT32)sizeof (EFI_BOOT_SERVICES),
    gST,
    gBS,
    gBS->CreateEventEx,
    &gBS
    ));

  if (!Forge) {
    return EFI_SUCCESS;
  }

  //
  // Already too new.
  //
  if (gST->Hdr.Revision >= EFI_2_30_SYSTEM_TABLE_REVISION) {
    /////
    gBS->CreateEventEx  = OcCreateEventEx;
    DEBUG ((
      DEBUG_INFO,
      "OCDM: Retrash to gBS->CreateEventEx %p\n",
      gBS->CreateEventEx
      ));
    /////
    return EFI_ALREADY_STARTED;
  }

  if (gBS->Hdr.HeaderSize > OFFSET_OF (EFI_BOOT_SERVICES, CreateEventEx)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Trash) {
    Signature = *(UINT64 *)(&gBS->CreateEventEx);
    if (Signature != DXE_SERVICES_SIGNATURE) {
      DEBUG ((
        DEBUG_INFO,
        "OCDM: Aborting trash strategy 0x%016lX !=  0x%016lX\n",
        Signature,
        DXE_SERVICES_SIGNATURE
      ));
      return EFI_UNSUPPORTED;
    }
    DEBUG ((
      DEBUG_INFO,
      "OCDM: DXE signature 0x%016lX found, trashing for CreateEventEx\n",
      Signature
    ));
    NewBS = gBS;
  } else {
    NewBS = AllocateZeroPool (sizeof (EFI_BOOT_SERVICES));
    if (NewBS == NULL) {
      DEBUG ((DEBUG_INFO, "OCDM: Failed to allocate BS copy\n"));
      return EFI_OUT_OF_RESOURCES;
    }
  }

  CopyMem (NewBS, gBS, gBS->Hdr.HeaderSize);

  NewBS->CreateEventEx  = OcCreateEventEx;
  NewBS->Hdr.HeaderSize = sizeof (EFI_BOOT_SERVICES);
  NewBS->Hdr.Revision   = EFI_2_30_SYSTEM_TABLE_REVISION;
  NewBS->Hdr.CRC32      = 0;
  NewBS->Hdr.CRC32      = CalculateCrc32 (NewBS, NewBS->Hdr.HeaderSize);
  gBS                   = NewBS;

  DEBUG ((
    DEBUG_INFO,
    "OCDM: NewBS %p\n",
    gBS
    ));

  gST->BootServices = NewBS;
  gST->Hdr.Revision = EFI_2_30_SYSTEM_TABLE_REVISION;
  gST->Hdr.CRC32    = 0;
  gST->Hdr.CRC32    = CalculateCrc32 (gST, gST->Hdr.HeaderSize);

  ASSERT (NewBS->CreateEventEx == OcCreateEventEx);

  return EFI_SUCCESS;
}
