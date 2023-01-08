/** @file
  Override firmware lock on EFI era iMac and Mac Pro.

  Copyright (c) 2023, Bmju. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Uefi.h>
#include <Library/UefiLib.h>

#include <Protocol/Runtime.h>
#include "../../UDK/MdeModulePkg/Core/Dxe/Event/Event.h"

#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/OcVariableLib.h>

#include <Guid/HobList.h>

#define EFI_EVENT_SIGNAL_EXIT_BOOT_SERVICES     0x00000201
#define EFI_EVENT_SIGNAL_LEGACY_BOOT            0x00000204

STATIC UINTN mFailedSignature = 0;
STATIC UINTN mMatchedTpl = 0;
STATIC UINTN mTrapMatchedTpl = 0;

STATIC EFI_GET_MEMORY_SPACE_MAP mOriginalGetMemorySpaceMap;


STATIC
VOID
LogStage (
  CHAR8       Stage,
  UINTN       Status
  )
{
  CHAR16  Name[] = L"firmware-unlock-0";

  Name[L_STR_LEN (Name) - 1] = Stage;
  OcSetSystemVariable (
    Name,
    OPEN_CORE_NVRAM_ATTR,
    sizeof (Status),
    &Status,
    NULL
    );
}

STATIC
VOID
LogGuid (
  CHAR8       Stage,
  EFI_GUID    *Guid
  )
{
  CHAR16  Name[] = L"hob-guid-0";

  Name[L_STR_LEN (Name) - 1] = Stage;
  OcSetSystemVariable (
    Name,
    OPEN_CORE_NVRAM_ATTR,
    sizeof (*Guid),
    Guid,
    NULL
    );
}

///////////////////////
/**
  Returns the type of a HOB.

  This macro returns the HobType field from the HOB header for the
  HOB specified by HobStart.

  @param  HobStart   A pointer to a HOB.

  @return HobType.

**/
#define GET_HOB_TYPE(HobStart) \
  ((*(EFI_HOB_GENERIC_HEADER **)&(HobStart))->HobType)

/**
  Returns the length, in bytes, of a HOB.

  This macro returns the HobLength field from the HOB header for the
  HOB specified by HobStart.

  @param  HobStart   A pointer to a HOB.

  @return HobLength.

**/
#define GET_HOB_LENGTH(HobStart) \
  ((*(EFI_HOB_GENERIC_HEADER **)&(HobStart))->HobLength)

/**
  Returns a pointer to the next HOB in the HOB list.

  This macro returns a pointer to HOB that follows the
  HOB specified by HobStart in the HOB List.

  @param  HobStart   A pointer to a HOB.

  @return A pointer to the next HOB in the HOB list.

**/
#define GET_NEXT_HOB(HobStart) \
  (VOID *)(*(UINT8 **)&(HobStart) + GET_HOB_LENGTH (HobStart))

/**
  Determines if a HOB is the last HOB in the HOB list.

  This macro determine if the HOB specified by HobStart is the
  last HOB in the HOB list.  If HobStart is last HOB in the HOB list,
  then TRUE is returned.  Otherwise, FALSE is returned.

  @param  HobStart   A pointer to a HOB.

  @retval TRUE       The HOB specified by HobStart is the last HOB in the HOB list.
  @retval FALSE      The HOB specified by HobStart is not the last HOB in the HOB list.

**/
#define END_OF_HOB_LIST(HobStart)  (GET_HOB_TYPE (HobStart) == (UINT16)EFI_HOB_TYPE_END_OF_HOB_LIST)

/**
  Returns a pointer to data buffer from a HOB of type EFI_HOB_TYPE_GUID_EXTENSION.

  This macro returns a pointer to the data buffer in a HOB specified by HobStart.
  HobStart is assumed to be a HOB of type EFI_HOB_TYPE_GUID_EXTENSION.

  @param   GuidHob   A pointer to a HOB.

  @return  A pointer to the data buffer in a HOB.

**/
#define GET_GUID_HOB_DATA(HobStart) \
  (VOID *)(*(UINT8 **)&(HobStart) + sizeof (EFI_HOB_GUID_TYPE))

/**
  Returns the size of the data buffer from a HOB of type EFI_HOB_TYPE_GUID_EXTENSION.

  This macro returns the size, in bytes, of the data buffer in a HOB specified by HobStart.
  HobStart is assumed to be a HOB of type EFI_HOB_TYPE_GUID_EXTENSION.

  @param   GuidHob   A pointer to a HOB.

  @return  The size of the data buffer.
**/
#define GET_GUID_HOB_DATA_SIZE(HobStart) \
  (UINT16)(GET_HOB_LENGTH (HobStart) - sizeof (EFI_HOB_GUID_TYPE))

STATIC VOID  *mHobList = NULL;

/**
  Returns the pointer to the HOB list.

  This function returns the pointer to first HOB in the list.
  For PEI phase, the PEI service GetHobList() can be used to retrieve the pointer
  to the HOB list.  For the DXE phase, the HOB list pointer can be retrieved through
  the EFI System Table by looking up theHOB list GUID in the System Configuration Table.
  Since the System Configuration Table does not exist that the time the DXE Core is
  launched, the DXE Core uses a global variable from the DXE Core Entry Point Library
  to manage the pointer to the HOB list.

  If the pointer to the HOB list is NULL, then ASSERT().

  This function also caches the pointer to the HOB list retrieved.

  @return The pointer to the HOB list.

**/
STATIC
VOID *
EFIAPI
GetHobList (
  VOID
  )
{
  EFI_STATUS  Status;

  if (mHobList == NULL) {
    Status = EfiGetSystemConfigurationTable (&gEfiHobListGuid, &mHobList);
    ASSERT_EFI_ERROR (Status);
    ASSERT (mHobList != NULL);
  }

  return mHobList;
}

/**
  Returns the next instance of a HOB type from the starting HOB.

  This function searches the first instance of a HOB type from the starting HOB pointer.
  If there does not exist such HOB type from the starting HOB pointer, it will return NULL.
  In contrast with macro GET_NEXT_HOB(), this function does not skip the starting HOB pointer
  unconditionally: it returns HobStart back if HobStart itself meets the requirement;
  caller is required to use GET_NEXT_HOB() if it wishes to skip current HobStart.

  If HobStart is NULL, then ASSERT().

  @param  Type          The HOB type to return.
  @param  HobStart      The starting HOB pointer to search from.

  @return The next instance of a HOB type from the starting HOB.

**/
STATIC
VOID *
EFIAPI
GetNextHob (
  IN UINT16      Type,
  IN CONST VOID  *HobStart
  )
{
  EFI_PEI_HOB_POINTERS  Hob;
  // STATIC UINTN TempIndex = 0;

  ASSERT (HobStart != NULL);

  Hob.Raw = (UINT8 *)HobStart;
  //
  // Parse the HOB list until end of list or matching type is found.
  //
  while (!END_OF_HOB_LIST (Hob)) {
    // LogStage ('B', TempIndex++);
    if (Hob.Header->HobType == Type) {
      return Hob.Raw;
    }

    Hob.Raw = GET_NEXT_HOB (Hob);
  }

  return NULL;
}

/**
  Returns the first instance of a HOB type among the whole HOB list.

  This function searches the first instance of a HOB type among the whole HOB list.
  If there does not exist such HOB type in the HOB list, it will return NULL.

  If the pointer to the HOB list is NULL, then ASSERT().

  @param  Type          The HOB type to return.

  @return The next instance of a HOB type from the starting HOB.

**/
#if 0
STATIC
VOID *
EFIAPI
GetFirstHob (
  IN UINT16  Type
  )
{
  VOID  *HobList;

  HobList = GetHobList ();
  return GetNextHob (Type, HobList);
}
#endif

/**
  Returns the next instance of the matched GUID HOB from the starting HOB.

  This function searches the first instance of a HOB from the starting HOB pointer.
  Such HOB should satisfy two conditions:
  its HOB type is EFI_HOB_TYPE_GUID_EXTENSION and its GUID Name equals to the input Guid.
  If there does not exist such HOB from the starting HOB pointer, it will return NULL.
  Caller is required to apply GET_GUID_HOB_DATA () and GET_GUID_HOB_DATA_SIZE ()
  to extract the data section and its size information, respectively.
  In contrast with macro GET_NEXT_HOB(), this function does not skip the starting HOB pointer
  unconditionally: it returns HobStart back if HobStart itself meets the requirement;
  caller is required to use GET_NEXT_HOB() if it wishes to skip current HobStart.

  If Guid is NULL, then ASSERT().
  If HobStart is NULL, then ASSERT().

  @param  Guid          The GUID to match with in the HOB list.
  @param  HobStart      A pointer to a Guid.

  @return The next instance of the matched GUID HOB from the starting HOB.

**/
STATIC
VOID *
EFIAPI
GetNextGuidHob (
  IN CONST EFI_GUID  *Guid,
  IN CONST VOID      *HobStart
  )
{
  EFI_PEI_HOB_POINTERS  GuidHob;
  UINTN                 TempOffset;

  LogStage ('A', 0);
  TempOffset  = 0;
  GuidHob.Raw = (UINT8 *)HobStart;
  while ((GuidHob.Raw = GetNextHob (EFI_HOB_TYPE_GUID_EXTENSION, GuidHob.Raw)) != NULL) {
    if (CompareGuid (Guid, &GuidHob.Guid->Name)) {
      LogStage ('C', 0);
      break;
    }

    LogGuid ('0' + (TempOffset++), &GuidHob.Guid->Name);

    GuidHob.Raw = GET_NEXT_HOB (GuidHob);
  }

  return GuidHob.Raw;
}

/**
  Returns the first instance of the matched GUID HOB among the whole HOB list.

  This function searches the first instance of a HOB among the whole HOB list.
  Such HOB should satisfy two conditions:
  its HOB type is EFI_HOB_TYPE_GUID_EXTENSION and its GUID Name equals to the input Guid.
  If there does not exist such HOB from the starting HOB pointer, it will return NULL.
  Caller is required to apply GET_GUID_HOB_DATA () and GET_GUID_HOB_DATA_SIZE ()
  to extract the data section and its size information, respectively.

  If the pointer to the HOB list is NULL, then ASSERT().
  If Guid is NULL, then ASSERT().

  @param  Guid          The GUID to match with in the HOB list.

  @return The first instance of the matched GUID HOB among the whole HOB list.

**/
STATIC
VOID *
EFIAPI
GetFirstGuidHob (
  IN CONST EFI_GUID  *Guid
  )
{
  VOID  *HobList;

  HobList = GetHobList ();
  return GetNextGuidHob (Guid, HobList);
}
///////////////////////

VOID
EFIAPI
MyEvent (
  IN  EFI_EVENT                Event,
  IN  VOID                     *Context
  )
{
  LogStage ('Y', EFI_SUCCESS);
  gBS->CloseEvent (Event);
}

VOID
LogEvent (
  IN IEVENT      *IEvent,
  BOOLEAN        Trapped
  )
{
  // STATIC UINTN       TempOffset = 0;
  // LogStage ((Trapped ? 'A' : '0') + (TempOffset++), IEvent->NotifyTpl);
}

STATIC EFI_CREATE_EVENT mOldCreateEvent;

EFI_STATUS
EFIAPI
WrappedCreateEvent (
  IN  UINT32                       Type,
  IN  EFI_TPL                      NotifyTpl,
  IN  EFI_EVENT_NOTIFY             NotifyFunction,
  IN  VOID                         *NotifyContext,
  OUT EFI_EVENT                    *Event
  )
{
  EFI_STATUS  Status;
  IEVENT      *IEvent;

  Status = mOldCreateEvent (
    Type,
    NotifyTpl,
    NotifyFunction,
    NotifyContext,
    Event
  );

  if (!EFI_ERROR (Status)
    && NotifyTpl == (EFI_EVENT_SIGNAL_LEGACY_BOOT | EFI_EVENT_SIGNAL_EXIT_BOOT_SERVICES)) {
    mTrapMatchedTpl++;
    IEvent = *Event;
    LogEvent (IEvent, TRUE);
  }

  return Status;
}

STATIC
VOID
WrapCreateEvent (
  VOID
  )
{
  mOldCreateEvent = gBS->CreateEvent;
  gBS->CreateEvent = WrappedCreateEvent;
}

typedef struct {
  BOOLEAN   LockFirmware;
} APPLE_FIRMWARE_LOCK_PROTOCOL;

STATIC EFI_GUID gAppleFirmwareLockProtocolGuid = { 0x31229466, 0xE00F, 0x4D83, { 0x88, 0x38, 0x51, 0xFE, 0x31, 0x05, 0x69, 0xC8 }};

STATIC
VOID
LogFirmwareUnlockStatus (
  VOID
  )
{
  EFI_PEI_HOB_POINTERS                GuidHob;
  APPLE_FIRMWARE_LOCK_PROTOCOL        *LockProtocol;
  UINTN                               UnprotectProtocolSize;

  GuidHob.Raw               = GetFirstGuidHob (&gAppleFirmwareLockProtocolGuid);
  LogStage('G', GuidHob.Raw != NULL);
  if (GuidHob.Raw != NULL) {
    LockProtocol            = GET_GUID_HOB_DATA (GuidHob.Guid);
    UnprotectProtocolSize   = GET_GUID_HOB_DATA_SIZE (GuidHob.Guid);
    LogStage('F', (UINTN)LockProtocol);
    LogStage('S', UnprotectProtocolSize);
    LogStage('L', LockProtocol->LockFirmware);
    LockProtocol->LockFirmware = FALSE;
    LogStage('M', LockProtocol->LockFirmware);
  }
}

STATIC
VOID
LogPCH (
  VOID
  )
{
  UINTN Base;
  UINT32 Pr0;
  UINT32 Pr1;
  UINT32 Pr2;
  UINT32 Pr3;
  UINT32 Pr4;
  UINT16 Lock;
  Base = *((UINT32 *) 0xE00F80F0);
  Base &= 0xFFFFFFFE;
  Pr0 = *((UINT32 *)(Base + 0x3874));
  Pr1 = *((UINT32 *)(Base + 0x3878));
  Pr2 = *((UINT32 *)(Base + 0x387C));
  Pr3 = *((UINT32 *)(Base + 0x3880));
  Pr4 = *((UINT32 *)(Base + 0x3884));
  Lock = *((UINT16 *)(Base + 0x3804));
  LogStage ('0', Pr0);
  LogStage ('1', Pr1);
  LogStage ('2', Pr2);
  LogStage ('3', Pr3);
  LogStage ('4', Pr4);
  LogStage ('l', Lock);
}

STATIC
EFI_STATUS
EFIAPI
WrappedGetMemorySpaceMap (
  OUT UINTN                            *NumberOfDescriptors,
  OUT EFI_GCD_MEMORY_SPACE_DESCRIPTOR  **MemorySpaceMap
  )
{
  STATIC UINTN  mGetMemorySpaceMapAccessCount = 0;

  if (mGetMemorySpaceMapAccessCount++ == 1) {
    LogStage ('f', mFailedSignature);
    LogStage ('m', mMatchedTpl);
    LogStage ('t', mTrapMatchedTpl);
    LogFirmwareUnlockStatus ();
    // LogPCH ();
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
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT   Event;
  EFI_TPL     OldTpl;
  LIST_ENTRY  *Link;
  LIST_ENTRY  *Head;
  IEVENT      *IEvent;

  OcVariableInit (FALSE);

  LogStage ('X', EFI_SUCCESS);

  LogPCH ();

  //
  // Event with same properties as the one we must patch.
  //
  gBS->CreateEvent (
    EFI_EVENT_SIGNAL_LEGACY_BOOT | EFI_EVENT_SIGNAL_EXIT_BOOT_SERVICES,
    TPL_NOTIFY,
    MyEvent,
    NULL,
    &Event
  );

  //
  // Protect event queue from modification while we scan it.
  //
  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  IEvent = Event;
  Head = &IEvent->SignalLink;
  for (Link = Head->ForwardLink; Link != Head; Link = Link->ForwardLink) {
    //
    // We disable DEBUG_ASSERT_ENABLED even on non-RELEASE builds for this module,
    // as otherwise we will generate one as we pass gEventSignalQueue.
    //
    IEvent = CR (Link, IEVENT, SignalLink, EVENT_SIGNATURE);

    if (IEvent->Signature != EVENT_SIGNATURE) {
      mFailedSignature++;
    } else {
      LogEvent (IEvent, FALSE);
      if (IEvent->NotifyTpl == (EFI_EVENT_SIGNAL_LEGACY_BOOT | EFI_EVENT_SIGNAL_EXIT_BOOT_SERVICES)) {
        mMatchedTpl++;
      }
    }
  }

  gBS->RestoreTPL (OldTpl);

  WrapGetMemorySpaceMap ();
  WrapCreateEvent ();

  return EFI_SUCCESS;
}
