/** @file
  Boot entry protocol handler for PXE and HTTP Boot.

  Copyright (c) 2024, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "NetworkBootInternal.h"

typedef struct {
  EFI_GUID          *OwnerGuid;
  UINTN             CertSize;
  CHAR8             *CertData;
  BOOLEAN           AddThisCert;
} CERT_INFO;

#define ENROLL_CERT L"--enroll-cert"

BOOLEAN gRequireHttpsUri;

STATIC BOOLEAN mAllowPxeBoot;
STATIC BOOLEAN mAllowHttpBoot;
STATIC BOOLEAN mAllowIpv4;
STATIC BOOLEAN mAllowIpv6;
STATIC CHAR16  *mHttpBootUri;

STATIC CHAR16 PxeBootId[]  = L"PXE Boot IPv";
STATIC CHAR16 HttpBootId[] = L"HTTP Boot IPv";

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

  if (Entry->UnmanagedBootDevicePath != NULL) {
    FreePool (Entry->UnmanagedBootDevicePath);
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
  BOOLEAN           IsIPv4,
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
  PickerEntry->UnmanagedBootDevicePath = NewDevicePath;

  if (IsHttpBoot) {
    PickerEntry->CustomRead = HttpBootCustomRead;
    PickerEntry->CustomFree = HttpBootCustomFree;
    PickerEntry->Flavour = IsIPv4 ? OC_FLAVOUR_HTTP_BOOT4 : OC_FLAVOUR_HTTP_BOOT6;
  } else {
    PickerEntry->CustomRead = PxeBootCustomRead;
    PickerEntry->Flavour = IsIPv4 ? OC_FLAVOUR_PXE_BOOT4 : OC_FLAVOUR_PXE_BOOT6;
  }

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
  CHAR16            *IdStr;
  OC_FLEX_ARRAY     *FlexPickerEntries;
  BOOLEAN           IsIPv4;
  BOOLEAN           IsHttpBoot;

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
      DebugPrintDevicePathForHandle (DEBUG_INFO, "NTBT: LoadFile handle not PXE/HTTP boot DP", HandleBuffer[Index]);
    } else {
      //
      // Use fixed format network description which we control as shortcut
      // to identify PXE/HTTP and IPv4/6.
      //
      if ((IdStr = StrStr (NetworkDescription, PxeBootId)) != NULL) {
        IsIPv4 = IdStr[STRLEN(PxeBootId)] == L'4';
        ASSERT (IsIPv4 || (IdStr[STRLEN(PxeBootId)] == L'6'));
        IsHttpBoot = FALSE;
      } else if ((IdStr = StrStr (NetworkDescription, HttpBootId)) != NULL) {
        IsIPv4 = IdStr[STRLEN(HttpBootId)] == L'4';
        ASSERT (IsIPv4 || (IdStr[STRLEN(HttpBootId)] == L'6'));
        IsHttpBoot = TRUE;
      }

      if (IdStr != NULL && ((IsIPv4 && mAllowIpv4) || (!IsIPv4 && mAllowIpv6))) {
        DEBUG ((DEBUG_INFO, "NTBT: Adding %s\n", NetworkDescription));
        Status = InternalAddEntry (
                    FlexPickerEntries,
                    NetworkDescription,
                    HandleBuffer[Index],
                    IsHttpBoot ? mHttpBootUri : NULL,
                    IsIPv4,
                    IsHttpBoot
                  );
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

EFI_STATUS
EnrollCerts (
  OC_FLEX_ARRAY        *ParsedLoadOptions
  )
{
  EFI_STATUS      Status;
  UINTN           Index;
  UINTN           Index2;
  UINTN           CertIndex;
  UINTN           CertCount;
  UINTN           Pass;
  UINTN           CertSize;
  OC_PARSED_VAR   *Option;
  CERT_INFO       *Certs;
  BOOLEAN         Removed;

  //
  // Find certs in options.
  //
  Certs   = NULL;
  Status  = EFI_SUCCESS;
  for (Pass = 0; Pass <= 1; ++Pass) {
    CertCount = 0;
    for (Index = 0; Index < ParsedLoadOptions->Count; ++Index) {
      Option = OcFlexArrayItemAt (ParsedLoadOptions, Index);
      if ( OcUnicodeStartsWith (Option->Unicode.Name, ENROLL_CERT, TRUE)
        && ( (Option->Unicode.Name[L_STR_LEN (ENROLL_CERT)] == CHAR_NULL)
          || (Option->Unicode.Name[L_STR_LEN (ENROLL_CERT)] == L':')
          )
        )
      {
        CertIndex = CertCount++;

        if (Pass == 1) {
          Certs[CertIndex].OwnerGuid = AllocateZeroPool (sizeof(EFI_GUID));
          if (Certs[CertIndex].OwnerGuid == NULL) {
            Status = EFI_OUT_OF_RESOURCES;
            break;
          }

          //
          // Use all zeros GUID if no user value supplied.
          //
          if (Option->Unicode.Name[L_STR_LEN (ENROLL_CERT)] == L':') {
            Status = StrToGuid (&Option->Unicode.Name[L_STR_LEN (ENROLL_CERT L":")], Certs[CertIndex].OwnerGuid);
            if (EFI_ERROR (Status)) {
              DEBUG ((DEBUG_WARN, "NTBT: Cannot parse cert owner GUID from %s - %r\n", Option->Unicode.Name, Status));
              break;
            }
          }

          //
          // Empty cert value forces clear for owner GUID.
          //
          if (Option->Unicode.Value != NULL) {
            //
            // We do not include the terminating '\0' in the stored certificate,
            // which matches how stored by e.g. OVMF when loaded from file;
            // but we must allocate space for '\0' for Unicode to ASCII conversion.
            //
            CertSize = StrLen (Option->Unicode.Value);
            Certs[CertIndex].CertData = AllocateZeroPool (CertSize + 1);
            if (Certs[CertIndex].CertData == NULL) {
              Status = EFI_OUT_OF_RESOURCES;
              break;
            }
            Certs[CertIndex].CertSize = CertSize;
            UnicodeStrToAsciiStrS (Option->Unicode.Value, Certs[CertIndex].CertData, CertSize + 1);
          }
        }
      }
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    if (Pass == 0) {
      if (CertCount == 0) {
        break;
      }

      Certs = AllocateZeroPool (CertCount * sizeof (CERT_INFO));
      if (Certs == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }
    }
  }

  //
  // Work out if any certs are missing; clear down and re-apply everything
  // for a given owner GUID if any certs for it are missing.
  //
  if (!EFI_ERROR (Status) && Certs != NULL) {
    Removed = FALSE;

    for (Index = 0; Index < CertCount; ++Index) {
      if (Certs[Index].AddThisCert) {
        continue;
      }

      if (Certs[Index].CertData == NULL) {
        Status = EFI_SUCCESS;
      } else {
        Status = CertIsPresent (
              EFI_TLS_CA_CERTIFICATE_VARIABLE,
              &gEfiTlsCaCertificateGuid,
              Certs[Index].OwnerGuid,
              Certs[Index].CertSize,
              Certs[Index].CertData
        );

        if (EFI_ERROR (Status) && (Status != EFI_ALREADY_STARTED)) {
          DEBUG ((DEBUG_INFO, "NTBT: Error checking for cert presence - %r\n", Status));
          break;
        }
      }
      
      if (Status == EFI_ALREADY_STARTED) {
        Status = EFI_SUCCESS;
      } else {
        DEBUG ((
          DEBUG_INFO,
          "NTBT: %a clearing existing certs for owner GUID %g\n",
          Certs[Index].CertData == NULL ? "Empty cert data forces" : "Cert not present,",
          Certs[Index].OwnerGuid
        ));
        Status = DeleteCertsForOwner (
          EFI_TLS_CA_CERTIFICATE_VARIABLE,
          &gEfiTlsCaCertificateGuid,
          Certs[Index].OwnerGuid
        );
        if (EFI_ERROR (Status)) {
          DEBUG ((
            DEBUG_INFO,
            "NTBT: Error clearing certs - %r\n",
            Status
          ));
          break;
        }
        Removed = TRUE;
        Certs[Index].AddThisCert = TRUE;
        for (Index2 = 0; Index2 < CertCount; ++Index2) {
          if ( (Index2 != Index)
            && !Certs[Index2].AddThisCert
            && CompareGuid (Certs[Index].OwnerGuid, Certs[Index2].OwnerGuid)
          ) {
            Certs[Index2].AddThisCert = TRUE;
          }
        }
      }
    }

    if (!EFI_ERROR (Status)) {
      if (!Removed) {
        DEBUG ((DEBUG_INFO, "NTBT: All certs already present\n"));
      } else {
        for (Index = 0; Index < CertCount; ++Index) {
          if (Certs[Index].AddThisCert && (Certs[Index].CertData != NULL)) {
            DEBUG ((DEBUG_INFO, "NTBT: Adding cert for owner GUID %g\n", Certs[Index].OwnerGuid));
            Status = EnrollX509toVariable (
              EFI_TLS_CA_CERTIFICATE_VARIABLE,
              &gEfiTlsCaCertificateGuid,
              Certs[Index].OwnerGuid,
              Certs[Index].CertSize,
              Certs[Index].CertData
            );
            if (EFI_ERROR (Status)) {
              break;
            }
          }
        }
      }
    }
  }

  //
  // Free resources.
  //
  if (Certs != NULL) {
    for (Index = 0; Index < CertCount; ++Index) {
      if (Certs[Index].OwnerGuid != NULL) {
        FreePool (Certs[Index].OwnerGuid);
      }
      if (Certs[Index].CertData != NULL) {
        FreePool (Certs[Index].CertData);
      }
    }
    FreePool (Certs);
  }

  return Status;
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

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mAllowIpv4         = FALSE;
  mAllowIpv6         = FALSE;
  mAllowPxeBoot     = FALSE;
  mAllowHttpBoot    = FALSE;
  gRequireHttpsUri  = FALSE;
  mHttpBootUri      = NULL;

  Status = OcParseLoadOptions (LoadedImage, &ParsedLoadOptions);
  if (EFI_ERROR (Status)) {
    if (Status != EFI_NOT_FOUND) {
      return Status;
    }
    Status = EFI_SUCCESS;
  } else {
    //
    // e.g. --https --uri=https://imageserver.org/OpenShell.efi
    //
    mAllowIpv4         = OcHasParsedVar (ParsedLoadOptions, L"-4", OcStringFormatUnicode);
    mAllowIpv6         = OcHasParsedVar (ParsedLoadOptions, L"-6", OcStringFormatUnicode);
    mAllowPxeBoot     = OcHasParsedVar (ParsedLoadOptions, L"--pxe", OcStringFormatUnicode);
    mAllowHttpBoot    = OcHasParsedVar (ParsedLoadOptions, L"--http", OcStringFormatUnicode);
    gRequireHttpsUri  = OcHasParsedVar (ParsedLoadOptions, L"--https", OcStringFormatUnicode);

    TempUri = NULL;
    OcParsedVarsGetUnicodeStr (ParsedLoadOptions, L"--uri", &TempUri);
    if (TempUri != NULL) {
      mHttpBootUri = AllocateCopyPool (StrSize (TempUri), TempUri);
      if (mHttpBootUri == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
      }
    }

    if (!EFI_ERROR (Status)) {
      Status = EnrollCerts (ParsedLoadOptions);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_WARN, "NTBT: Failed to enroll certs - %r\n", Status));
      }

      DEBUG_CODE_BEGIN ();
      LogInstalledCerts (EFI_TLS_CA_CERTIFICATE_VARIABLE, &gEfiTlsCaCertificateGuid);
      DEBUG_CODE_END ();
    }
  }

  if (!EFI_ERROR (Status)) {
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
          Status = EFI_INVALID_PARAMETER;
        }
      }
    }

    if (!EFI_ERROR (Status)) {
      Status = gBS->InstallMultipleProtocolInterfaces (
                      &ImageHandle,
                      &gOcBootEntryProtocolGuid,
                      &mNetworkBootEntryProtocol,
                      NULL
                      );
    }
  }

  if (ParsedLoadOptions != NULL) {
    OcFlexArrayFree (&ParsedLoadOptions);
  }
  
  if (EFI_ERROR (Status) && mHttpBootUri != NULL) {
    FreePool (mHttpBootUri);
  }

  return Status;
}
