/** @file
  UEFI debug log wrapper.

  Copyright (c) 2022, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/OcDirectResetLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/VarCheck.h>

EFI_GET_VARIABLE                        mGetVariable;
EFI_GET_NEXT_VARIABLE_NAME              mGetNextVariableName;
EFI_SET_VARIABLE                        mSetVariable;

EDKII_VAR_CHECK_VARIABLE_PROPERTY_GET   mVarCheckVariablePropertyGet;
EDKII_VAR_CHECK_VARIABLE_PROPERTY_SET   mVarCheckVariablePropertySet;

STATIC EFI_GUID  mEfiTimeGuid = {
  0x9D0DA369, 0x540B, 0x46F8, { 0x85, 0xA0, 0x2B, 0x5F, 0x2C, 0x30, 0x1E, 0x15 }
};

EFI_LOCATE_HANDLE_BUFFER  mOriginalLocateHandleBuffer;
EFI_LOCATE_PROTOCOL       mOriginalLocateProtocol;

STATIC
EFI_STATUS
EFIAPI
WrappedLocateHandleBuffer (
  IN     EFI_LOCATE_SEARCH_TYPE       SearchType,
  IN     EFI_GUID                     *Protocol       OPTIONAL,
  IN     VOID                         *SearchKey      OPTIONAL,
  OUT    UINTN                        *NoHandles,
  OUT    EFI_HANDLE                   **Buffer
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN Nested = FALSE;

  if (Nested) {
    return mOriginalLocateHandleBuffer (SearchType, Protocol, SearchKey, NoHandles, Buffer);
  }

  Nested = TRUE;

  DEBUG ((DEBUG_INFO, "WRAP: > LocateHandleBuffer %u %g %p %u %p\n", SearchType, Protocol, SearchKey, *NoHandles, *Buffer));
  Status = mOriginalLocateHandleBuffer (SearchType, Protocol, SearchKey, NoHandles, Buffer);
  DEBUG ((DEBUG_INFO, "WRAP: < LocateHandleBuffer %u %g %p %u %p - %r\n", SearchType, Protocol, SearchKey, *NoHandles, *Buffer, Status));

  Nested = FALSE;

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
WrappedLocateProtocol (
  IN  EFI_GUID  *Protocol,
  IN  VOID      *Registration  OPTIONAL,
  OUT VOID      **Interface
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN Nested = FALSE;

  if (Nested) {
    return mOriginalLocateProtocol (Protocol, Registration, Interface);
  }

  Nested = TRUE;

  DEBUG ((DEBUG_INFO, "WRAP: > LocateProtocol %g %p %p\n", Protocol, Registration, *Interface));
  Status = mOriginalLocateProtocol (Protocol, Registration, Interface);
  DEBUG ((DEBUG_INFO, "WRAP: < LocateProtocol %g %p %p - %r\n", Protocol, Registration, *Interface, Status));

  Nested = FALSE;

  return Status;
}

STATIC
VOID
WrapLocateProtocol (
  VOID
  )
{
  mOriginalLocateHandleBuffer = gBS->LocateHandleBuffer;
  gBS->LocateHandleBuffer = WrappedLocateHandleBuffer;

  mOriginalLocateProtocol = gBS->LocateProtocol;
  gBS->LocateProtocol = WrappedLocateProtocol;
}

STATIC
VOID
UnwrapLocateProtocol (
  VOID
  )
{
  gBS->LocateHandleBuffer = mOriginalLocateHandleBuffer;
  gBS->LocateProtocol = mOriginalLocateProtocol;
}

STATIC
EFI_STATUS
EFIAPI
WrappedGetVariable (
  IN     CHAR16                      *VariableName,
  IN     EFI_GUID                    *VendorGuid,
  OUT    UINT32                      *Attributes     OPTIONAL,
  IN OUT UINTN                       *DataSize,
  OUT    VOID                        *Data           OPTIONAL
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN Nested = FALSE;

  //
  // Ignore EfiTime, it is fetched continually.
  //
  if (Nested
    || (StrCmp (L"EfiTime", VariableName) == 0 && CompareGuid (&mEfiTimeGuid, VendorGuid))) {
    return mGetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  }

  Nested = TRUE;
  DEBUG ((DEBUG_INFO, "WRAP: > GetVariable %g:%s %p %u %p\n", VendorGuid, VariableName, Attributes, *DataSize, Data));
  Status = mGetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  DEBUG ((DEBUG_INFO, "WRAP: < GetVariable %g:%s 0x%X %u %p - %r\n", VendorGuid, VariableName, Attributes == NULL ? -1 : *Attributes, *DataSize, Data, Status));
  Nested = FALSE;

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
WrappedGetNextVariableName (
  IN OUT UINTN                    *VariableNameSize,
  IN OUT CHAR16                   *VariableName,
  IN OUT EFI_GUID                 *VendorGuid
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN Nested = FALSE;

  if (Nested) {
    return mGetNextVariableName (VariableNameSize, VariableName, VendorGuid);
  }

  Nested = TRUE;
  DEBUG ((DEBUG_INFO, "WRAP: > GetNextVariableName %u %g:%s\n", VariableNameSize, VendorGuid, VariableName));
  Status = mGetNextVariableName (VariableNameSize, VariableName, VendorGuid);
  DEBUG ((DEBUG_INFO, "WRAP: < GetNextVariableName %u %g:%s - %r\n", VariableNameSize, VendorGuid, VariableName, Status));
  Nested = FALSE;

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
WrappedSetVariable (
  IN  CHAR16                       *VariableName,
  IN  EFI_GUID                     *VendorGuid,
  IN  UINT32                       Attributes,
  IN  UINTN                        DataSize,
  IN  VOID                         *Data
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN Nested = FALSE;

  if (Nested) {
    return mSetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  }

  Nested = TRUE;
  DEBUG ((DEBUG_INFO, "WRAP: > SetVariable %g:%s 0x%X %u %p\n", VendorGuid, VariableName, Attributes, DataSize, Data));
  Status = mSetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  DEBUG ((DEBUG_INFO, "WRAP: < SetVariable - %r\n", Status));
  Nested = FALSE;

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
WrappedVarCheckVariablePropertyGet (
  IN CHAR16                         *Name,
  IN EFI_GUID                       *Guid,
  OUT VAR_CHECK_VARIABLE_PROPERTY   *VariableProperty
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN Nested = FALSE;

  if (Nested) {
    return mVarCheckVariablePropertyGet (Name, Guid, VariableProperty);
  }

  Nested = TRUE;
  DEBUG ((DEBUG_INFO, "WRAP: > VarCheckVariablePropertyGet %g:%s %p\n", Guid, Name, VariableProperty));
  Status = mVarCheckVariablePropertyGet (Name, Guid, VariableProperty);
  DEBUG ((DEBUG_INFO, "WRAP: < VarCheckVariablePropertyGet %g:%s %p - %r\n", Guid, Name, VariableProperty, Status));
  Nested = FALSE;

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
WrappedVarCheckVariablePropertySet (
  IN CHAR16                         *Name,
  IN EFI_GUID                       *Guid,
  IN VAR_CHECK_VARIABLE_PROPERTY    *VariableProperty
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN Nested = FALSE;

  if (Nested) {
    return mVarCheckVariablePropertySet (Name, Guid, VariableProperty);
  }

  Nested = TRUE;
  DEBUG ((DEBUG_INFO, "WRAP: > VarCheckVariablePropertySet %g:%s %p\n", Guid, Name, VariableProperty));
  Status = mVarCheckVariablePropertySet (Name, Guid, VariableProperty);
  DEBUG ((DEBUG_INFO, "WRAP: < VarCheckVariablePropertySet %g:%s %p - %r\n", Guid, Name, VariableProperty, Status));
  Nested = FALSE;

  return Status;
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
#if 1
  DirectResetCold ();
#else
  gBS->CloseEvent (Event);

  if (gRT->GetVariable == WrappedGetVariable) {
    gRT->GetVariable = mGetVariable;
  }

  if (gRT->GetNextVariableName == WrappedGetNextVariableName) {
    gRT->GetNextVariableName = mGetNextVariableName;
  }

  if (gRT->SetVariable == WrappedSetVariable) {
    gRT->SetVariable = mSetVariable;
  }
#endif
}

STATIC
VOID
WrapNvramVariables (
  VOID
  )
{
  mGetVariable = gRT->GetVariable;
  gRT->GetVariable = WrappedGetVariable;

  mGetNextVariableName = gRT->GetNextVariableName;
  gRT->GetNextVariableName = WrappedGetNextVariableName;

  mSetVariable = gRT->SetVariable;
  gRT->SetVariable = WrappedSetVariable;

  DEBUG ((DEBUG_INFO, "WRAP: NvramVariables wrapped\n"));
}

STATIC
VOID
WrapVarCheck (
  VOID
  )
{
  EFI_STATUS                     Status;
  UINTN                          Index;
  EFI_HANDLE                     *Handles;
  UINTN                          HandleCount;
  EDKII_VAR_CHECK_PROTOCOL       VarCheck;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEdkiiVarCheckProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
 
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "WRAP: Locate VarCheck protocol - %r\n", Status));
    return;
  }

  if (HandleCount != 1) {
    DEBUG ((DEBUG_WARN, "WRAP: VarCheck found %u handles, not wrapping!\n", HandleCount));
    FreePool (Handles);
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEdkiiVarCheckProtocolGuid,
                    (VOID **)&VarCheck
                    );
    if (!EFI_ERROR (Status)) {
      if (HandleCount == 1) {
        mVarCheckVariablePropertyGet = VarCheck.VariablePropertyGet;
        mVarCheckVariablePropertySet = VarCheck.VariablePropertySet;
      }
      VarCheck.VariablePropertyGet = WrappedVarCheckVariablePropertyGet;
      VarCheck.VariablePropertySet = WrappedVarCheckVariablePropertySet;

      DEBUG ((DEBUG_INFO, "WRAP: VarCheck %u/%u wrapped\n", Index, HandleCount));
    }

  }
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS      Status;
  EFI_EVENT       ExitBootServicesEvent;

  WrapNvramVariables ();
  WrapVarCheck ();

  Status = gBS->CreateEvent (
                  EVT_SIGNAL_EXIT_BOOT_SERVICES,
                  TPL_NOTIFY,
                  OnExitBootServices,
                  NULL,
                  &ExitBootServicesEvent
                  );

  return Status;
}
