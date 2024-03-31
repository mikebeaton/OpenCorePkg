/** @file
  Boot entry protocol handler for PXE and HTTP Boot.

  Copyright (c) 2024, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "NetworkBootInternal.h"

BOOLEAN gRequireHttpsUri;

STATIC BOOLEAN mAllowPxeBoot;
STATIC BOOLEAN mAllowHttpBoot;

STATIC CHAR16  *mHttpBootUri;

STATIC CHAR16 PxeBootId[]  = L"PXEv_";
STATIC CHAR16 HttpBootId[] = L"HTTPv_";

VOID
InternalFreePickerEntry (
  IN   OC_PICKER_ENTRY  *Entry
  )
{
  ASSERT (Entry != NULL);

  if (Entry == NULL) {
    return;
  }

  if (Entry->Id != NULL) {
    FreePool ((CHAR8 *)Entry->Id);
  }

  if (Entry->Name != NULL) {
    FreePool ((CHAR8 *)Entry->Name);
  }

  if (Entry->Path != NULL) {
    FreePool ((CHAR8 *)Entry->Path);
  }

  if (Entry->Arguments != NULL) {
    FreePool ((CHAR8 *)Entry->Arguments);
  }

  if (Entry->ExternalDevicePath != NULL) {
    FreePool (Entry->ExternalDevicePath);
  }
}

STATIC
VOID
EFIAPI
FreeNetworkBootEntries (
  IN   OC_PICKER_ENTRY  **Entries,
  IN   UINTN            NumEntries
  )
{
  UINTN  Index;

  ASSERT (Entries   != NULL);
  ASSERT (*Entries  != NULL);
  if ((Entries == NULL) || (*Entries == NULL)) {
    return;
  }

  for (Index = 0; Index < NumEntries; Index++) {
    InternalFreePickerEntry (&(*Entries)[Index]);
  }

  FreePool (*Entries);
  *Entries = NULL;
}

STATIC
EFI_STATUS
InternalAddEntry (
  OC_FLEX_ARRAY     *FlexPickerEntries,
  CHAR16            *Description,
  EFI_HANDLE        Handle,
  CHAR16            *HttpBootUri,
  BOOLEAN           IsHttpBoot
  )
{
  EFI_STATUS                Status;
  OC_PICKER_ENTRY           *PickerEntry;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *NewDevicePath;
  UINTN                     IdLen;

  Status = gBS->HandleProtocol (
    Handle,
    &gEfiDevicePathProtocolGuid,
    (VOID **)&DevicePath
  );
  if (EFI_ERROR(Status)) {
    DEBUG ((
      DEBUG_INFO,
      "NTBT: Missing device path - %r\n",
      Status
      ));
      return Status;
  }

  PickerEntry = OcFlexArrayAddItem (FlexPickerEntries);
  if (PickerEntry == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  IdLen = StrLen (Description);
  PickerEntry->Id = AllocatePool ((IdLen + 1) * sizeof (PickerEntry->Id[0]));
  if (PickerEntry->Id == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  UnicodeStrToAsciiStrS (Description, (CHAR8 *)PickerEntry->Id, IdLen + 1);

  PickerEntry->Name = AllocateCopyPool(IdLen + 1, PickerEntry->Id);
  if (PickerEntry->Name == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (IsHttpBoot && (HttpBootUri != NULL)) {
    Status = HttpBootAddUri (DevicePath, HttpBootUri, OcStringFormatUnicode, &NewDevicePath);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  } else {
    NewDevicePath = DuplicateDevicePath (DevicePath);
    if (NewDevicePath == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
  }
  PickerEntry->ExternalDevicePath = NewDevicePath;

  PickerEntry->CustomRead = NetworkBootCustomRead;
  PickerEntry->CustomFree = NetworkBootCustomFree;

  PickerEntry->Flavour = IsHttpBoot ? OC_FLAVOUR_HTTP_BOOT : OC_FLAVOUR_PXE_BOOT;

  //
  // Probably sensible on balance ... although not yet clear how it will interact with GUI-based firmware.
  //
  PickerEntry->TextMode = TRUE;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GetNetworkBootEntries (
  IN OUT          OC_PICKER_CONTEXT  *PickerContext,
  IN     CONST EFI_HANDLE            Device OPTIONAL,
  OUT       OC_PICKER_ENTRY          **Entries,
  OUT       UINTN                    *NumEntries
  )
{
  EFI_STATUS        Status;
  UINTN             HandleCount;
  EFI_HANDLE        *HandleBuffer;
  UINTN             Index;
  CHAR16            *NetworkDescription;
  OC_FLEX_ARRAY     *FlexPickerEntries;

  //
  // Here we produce custom entries only, not entries found on filesystems.
  //
  if (Device != NULL) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->LocateHandleBuffer (
    ByProtocol,
    &gEfiLoadFileProtocolGuid,
    NULL,
    &HandleCount,
    &HandleBuffer
  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "NTBT: Load file protocol - %r\n", Status));
    return Status;
  }

  FlexPickerEntries = OcFlexArrayInit (sizeof (OC_PICKER_ENTRY), (OC_FLEX_ARRAY_FREE_ITEM)InternalFreePickerEntry);
  if (FlexPickerEntries == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < HandleCount; ++Index) {
    NetworkDescription = BmGetNetworkDescription (HandleBuffer[Index]);
    if (NetworkDescription == NULL) {
      DEBUG ((
        DEBUG_INFO,
        "NTBT: Handle %u/%u not network boot, load file description '%s'\n",
        Index,
        HandleCount,
        BmGetLoadFileDescription (HandleBuffer[Index])
      ));
    } else {
      //
      // Use network description as shortcut to identify entry type.
      //
      if (mAllowPxeBoot && StrStr (NetworkDescription, PxeBootId)) {
        DEBUG ((DEBUG_INFO, "NTBT: Adding %s\n", NetworkDescription));
        Status = InternalAddEntry (FlexPickerEntries, NetworkDescription, HandleBuffer[Index], NULL, FALSE);
      } else if (mAllowHttpBoot && StrStr (NetworkDescription, HttpBootId)) {
        DEBUG ((DEBUG_INFO, "NTBT: Adding %s\n", NetworkDescription));
        Status = InternalAddEntry (FlexPickerEntries, NetworkDescription, HandleBuffer[Index], mHttpBootUri, TRUE);
      } else {
        DEBUG ((DEBUG_INFO, "NTBT: Ignoring %s\n", NetworkDescription));
      }

      FreePool (NetworkDescription);
    }

    if (EFI_ERROR (Status)) {
      break;
    }
  }

  FreePool (HandleBuffer);

  if (EFI_ERROR (Status)) {
    OcFlexArrayFree (&FlexPickerEntries);
    return Status;
  }

  OcFlexArrayFreeContainer (&FlexPickerEntries, (VOID **)Entries, NumEntries);

  if (NumEntries == 0) {
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

STATIC
OC_BOOT_ENTRY_PROTOCOL
  mNetworkBootEntryProtocol = {
  OC_BOOT_ENTRY_PROTOCOL_REVISION,
  GetNetworkBootEntries,
  FreeNetworkBootEntries,
  NULL
};

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                Status;
  EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
  OC_FLEX_ARRAY             *ParsedLoadOptions;
  CHAR16                    *TempUri;
  BOOLEAN                   AllowIpv4;
  BOOLEAN                   AllowIpv6;
  CHAR16                    IdChar;

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  AllowIpv4         = FALSE;
  AllowIpv6         = FALSE;
  mAllowPxeBoot     = FALSE;
  mAllowHttpBoot    = FALSE;
  gRequireHttpsUri  = FALSE;
  mHttpBootUri      = NULL;

  Status = OcParseLoadOptions (LoadedImage, &ParsedLoadOptions);
  if (EFI_ERROR (Status)) {
    if (Status != EFI_NOT_FOUND) {
      return Status;
    }
  } else {
    //
    // e.g. --https --uri=https://imageserver.org/OpenShell.efi
    //
    AllowIpv4         = OcHasParsedVar (ParsedLoadOptions, L"-4", OcStringFormatUnicode);
    AllowIpv6         = OcHasParsedVar (ParsedLoadOptions, L"-6", OcStringFormatUnicode);
    mAllowPxeBoot     = OcHasParsedVar (ParsedLoadOptions, L"--pxe", OcStringFormatUnicode);
    mAllowHttpBoot    = OcHasParsedVar (ParsedLoadOptions, L"--http", OcStringFormatUnicode);
    gRequireHttpsUri  = OcHasParsedVar (ParsedLoadOptions, L"--https", OcStringFormatUnicode);

    TempUri = NULL;
    OcParsedVarsGetUnicodeStr (ParsedLoadOptions, L"--uri", &TempUri);
    if (TempUri != NULL) {
      mHttpBootUri = AllocateCopyPool (StrSize (TempUri), TempUri);
      if (mHttpBootUri == NULL) {
        OcFlexArrayFree (&ParsedLoadOptions);
        return EFI_OUT_OF_RESOURCES;
      }
    }
  }

  if (AllowIpv4 == AllowIpv6) {
    IdChar = CHAR_NULL; ///< If neither (or both) are specified, allow both.
  } else if (AllowIpv4) {
    IdChar = L'4';
  } else {
    IdChar = L'6';
  }

  PxeBootId[L_STR_LEN (PxeBootId) - 1]    = IdChar;
  HttpBootId[L_STR_LEN (HttpBootId) - 1]  = IdChar;

  if (!gRequireHttpsUri && !mAllowHttpBoot && !mAllowPxeBoot) {
    mAllowHttpBoot = TRUE;
    mAllowPxeBoot  = TRUE;
  }

  if (gRequireHttpsUri) {
    mAllowHttpBoot = TRUE;
  }

  if (mHttpBootUri != NULL) {
    if (!mAllowHttpBoot) {
      DEBUG ((DEBUG_INFO, "NTBT: URI specified but HTTP boot is disabled\n"));
    } else {
      if (!HasValidUriProtocol (mHttpBootUri)) {
        OcFlexArrayFree (&ParsedLoadOptions);
        return EFI_INVALID_PARAMETER;
      }
    }
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gOcBootEntryProtocolGuid,
                  &mNetworkBootEntryProtocol,
                  NULL
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (ParsedLoadOptions != NULL) {
    OcFlexArrayFree (&ParsedLoadOptions);
  }

  return EFI_SUCCESS;
}
