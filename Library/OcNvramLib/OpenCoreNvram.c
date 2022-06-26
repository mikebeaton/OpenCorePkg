/** @file
  OpenCore driver.

Copyright (c) 2019, vit9696. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <Library/OcMainLib.h>

#include <Guid/OcVariable.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/OcCpuLib.h>
#include <Library/OcDeviceMiscLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcNvramLib.h>
#include <Library/OcSerializeLib.h>
#include <Library/OcStringLib.h>
#include <Library/OcVariableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/OcFirmwareRuntime.h>
#include <Protocol/OcVariableRuntime.h>

STATIC
VOID
OcReportVersion (
  IN OC_GLOBAL_CONFIG  *Config
  )
{
  CONST CHAR8  *Version;

  Version = OcMiscGetVersionString ();

  DEBUG ((DEBUG_INFO, "OCVAR: Current version is %a\n", Version));

  if ((Config->Misc.Security.ExposeSensitiveData & OCS_EXPOSE_VERSION_VAR) != 0) {
    OcSetSystemVariable (
      OC_VERSION_VARIABLE_NAME,
      OPEN_CORE_NVRAM_ATTR,
      AsciiStrLen (Version),
      (VOID *)Version,
      NULL
      );
  } else {
    OcSetSystemVariable (
      OC_VERSION_VARIABLE_NAME,
      OPEN_CORE_NVRAM_ATTR,
      L_STR_LEN ("UNK-000-0000-00-00"),
      "UNK-000-0000-00-00",
      NULL
      );
  }
}

EFI_STATUS
OcProcessVariableGuid (
  IN  CONST CHAR8            *AsciiVariableGuid,
  OUT GUID                   *VariableGuid,
  IN  OC_NVRAM_LEGACY_MAP    *Schema  OPTIONAL,
  OUT OC_NVRAM_LEGACY_ENTRY  **SchemaEntry  OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT32      GuidIndex;

  Status = AsciiStrToGuid (AsciiVariableGuid, VariableGuid);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OCVAR: Failed to convert NVRAM GUID %a - %r\n", AsciiVariableGuid, Status));
  }

  if (!EFI_ERROR (Status) && (Schema != NULL)) {
    for (GuidIndex = 0; GuidIndex < Schema->Count; ++GuidIndex) {
      if (AsciiStrCmp (AsciiVariableGuid, OC_BLOB_GET (Schema->Keys[GuidIndex])) == 0) {
        *SchemaEntry = Schema->Values[GuidIndex];
        return Status;
      }
    }

    DEBUG ((DEBUG_INFO, "OCVAR: Ignoring NVRAM GUID %a\n", AsciiVariableGuid));
    Status = EFI_SECURITY_VIOLATION;
  }

  return Status;
}

BOOLEAN
OcVariableIsAllowedBySchemaEntry (
  IN OC_NVRAM_LEGACY_ENTRY  *SchemaEntry,
  IN EFI_GUID               *VariableGuid OPTIONAL,
  IN CONST VOID             *VariableName,
  IN OC_STRING_FORMAT       StringFormat
  )
{
  BOOLEAN     IsAllowed;
  UINT32      VariableIndex;

  if (SchemaEntry == NULL) {
    return TRUE;
  }

  IsAllowed = FALSE;

  //
  // TODO: Consider optimising lookup if it causes problems...
  //
  for (VariableIndex = 0; VariableIndex < SchemaEntry->Count; ++VariableIndex) {
    if ((VariableIndex == 0) && (AsciiStrCmp ("*", OC_BLOB_GET (SchemaEntry->Values[VariableIndex])) == 0)) {
      IsAllowed = TRUE;
      break;
    }

    if ((StringFormat == OcStringFormatAscii)
      && AsciiStrCmp ((CONST CHAR8 *)VariableName, OC_BLOB_GET (SchemaEntry->Values[VariableIndex])) == 0) {
      IsAllowed = TRUE;
      break;
    }

    if ((StringFormat == OcStringFormatUnicode)
      && MixedStrCmp ((CONST CHAR16 *)VariableName, OC_BLOB_GET (SchemaEntry->Values[VariableIndex])) == 0) {
      IsAllowed = TRUE;
      break;
    }
  }

  if (!IsAllowed) {
    DEBUG ((DEBUG_INFO, "OCVAR: NVRAM %g:%s is not permitted\n", VariableGuid, VariableName));
  }

  return IsAllowed;
}

VOID
OcDirectSetNvramVariable (
  IN CONST CHAR8            *AsciiVariableName,
  IN EFI_GUID               *VariableGuid,
  IN UINT32                 Attributes,
  IN UINT32                 VariableSize,
  IN VOID                   *VariableData,
  IN OC_NVRAM_LEGACY_ENTRY  *SchemaEntry OPTIONAL,
  IN BOOLEAN                Overwrite
  )
{
  EFI_STATUS  Status;
  UINTN       OriginalVariableSize;
  CHAR16      *UnicodeVariableName;
  VOID        *OrgValue;
  UINTN       OrgSize;
  UINT32      OrgAttributes;


  if (!OcVariableIsAllowedBySchemaEntry (SchemaEntry, VariableGuid, AsciiVariableName, OcStringFormatAscii)) {
    return;
  }


  UnicodeVariableName = AsciiStrCopyToUnicode (AsciiVariableName, 0);

  if (UnicodeVariableName == NULL) {
    DEBUG ((DEBUG_WARN, "OCVAR: Failed to convert NVRAM variable name %a\n", AsciiVariableName));
    return;
  }

  OriginalVariableSize = 0;
  Status               = gRT->GetVariable (
                                UnicodeVariableName,
                                VariableGuid,
                                NULL,
                                &OriginalVariableSize,
                                NULL
                                );

  if ((Status == EFI_BUFFER_TOO_SMALL) && Overwrite) {
    Status = GetVariable3 (UnicodeVariableName, VariableGuid, &OrgValue, &OrgSize, &OrgAttributes);
    if (!EFI_ERROR (Status)) {
      //
      // Do not allow overwriting BS-only variables. Ideally we also check for NV attribute,
      // but it is not set by Duet.
      //
      if ((Attributes & (EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_BOOTSERVICE_ACCESS))
          == (EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_BOOTSERVICE_ACCESS))
      {
        Status = gRT->SetVariable (UnicodeVariableName, VariableGuid, 0, 0, 0);

        if (EFI_ERROR (Status)) {
          DEBUG ((
            DEBUG_INFO,
            "OCVAR: Failed to delete overwritten variable %g:%a - %r\n",
            VariableGuid,
            AsciiVariableName,
            Status
            ));
          Status = EFI_BUFFER_TOO_SMALL;
        }
      } else {
        DEBUG ((
          DEBUG_INFO,
          "OCVAR: Overwritten variable %g:%a has invalid attrs - %X\n",
          VariableGuid,
          AsciiVariableName,
          Attributes
          ));
        Status = EFI_BUFFER_TOO_SMALL;
      }

      FreePool (OrgValue);
    } else {
      DEBUG ((
        DEBUG_INFO,
        "OCVAR: Overwritten variable %g:%a has unknown attrs - %r\n",
        VariableGuid,
        AsciiVariableName,
        Status
        ));
      Status = EFI_BUFFER_TOO_SMALL;
    }
  }

  if (Status != EFI_BUFFER_TOO_SMALL) {
    Status = gRT->SetVariable (
                    UnicodeVariableName,
                    VariableGuid,
                    Attributes,
                    VariableSize,
                    VariableData
                    );
    DEBUG ((
      EFI_ERROR (Status) && VariableSize > 0 ? DEBUG_WARN : DEBUG_INFO,
      "OCVAR: Setting NVRAM %g:%a - %r\n",
      VariableGuid,
      AsciiVariableName,
      Status
      ));
  } else {
    DEBUG ((
      DEBUG_INFO,
      "OCVAR: Setting NVRAM %g:%a - ignored, exists\n",
      VariableGuid,
      AsciiVariableName,
      Status
      ));
  }

  FreePool (UnicodeVariableName);
}

STATIC
EFI_STATUS
InternalLocateVariableRuntimeProtocol (
  OC_VARIABLE_RUNTIME_PROTOCOL  **OcVariableRuntimeProtocol
  )
{
  EFI_STATUS                    Status;

  ASSERT (OcVariableRuntimeProtocol != NULL);

  Status = gBS->LocateProtocol (
                  &gOcVariableRuntimeProtocolGuid,
                  NULL,
                  (VOID **)OcVariableRuntimeProtocol
                  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCVAR: Locate emulated NVRAM protocol - %r\n", Status));
    return Status;
  }

  if ((*OcVariableRuntimeProtocol)->Revision != OC_VARIABLE_RUNTIME_PROTOCOL_REVISION) {
    DEBUG ((
      DEBUG_WARN,
      "OCVAR: Emulated NVRAM protocol incompatible revision %d != %d\n",
      (*OcVariableRuntimeProtocol)->Revision,
      OC_VARIABLE_RUNTIME_PROTOCOL_REVISION
      ));
    return EFI_UNSUPPORTED;
  }

  return Status;
}

STATIC
VOID
OcLoadLegacyNvram (
  IN OC_STORAGE_CONTEXT               *Storage,
  IN OC_GLOBAL_CONFIG                 *Config
  )
{
  EFI_STATUS                    Status;
  OC_VARIABLE_RUNTIME_PROTOCOL  *OcVariableRuntimeProtocol;
  OC_FIRMWARE_RUNTIME_PROTOCOL  *FwRuntime;
  OC_FWRT_CONFIG                FwrtConfig;

  Status = InternalLocateVariableRuntimeProtocol (&OcVariableRuntimeProtocol);
  if (EFI_ERROR (Status)) {
    return;
  }

  //
  // It is not really required to support boot var routing with emulated NVRAM (since there are
  // no firmware NVRAM boot vars used outside of OpenCore to avoid trashing), but having working
  // support is more convenient when switching back and forth between emulated and non-emulated
  // NVRAM, i.e. one less thing to have to remember to switch, since with this code everything
  // works as expected with or without RequestBootVarRouting. (Without it, boot entries do not
  // restore to the right place when RequestBootVarRouting is enabled.)
  // OpenRuntime.efi must be loaded early, but after OpenVariableRuntimeDxe.efi, for this to work.
  //
  if (Config->Uefi.Quirks.RequestBootVarRouting) {
    Status = gBS->LocateProtocol (
      &gOcFirmwareRuntimeProtocolGuid,
      NULL,
      (VOID **) &FwRuntime
      );

    if (!EFI_ERROR (Status) && FwRuntime->Revision == OC_FIRMWARE_RUNTIME_REVISION) {
      FwRuntime->GetCurrent (&FwrtConfig);
      if (FwrtConfig.BootVariableRedirect) {
        DEBUG ((DEBUG_INFO, "OCVAR: Found FW NVRAM, redirect already present %d\n", FwrtConfig.BootVariableRedirect));
        FwRuntime = NULL;
      } else {
        FwrtConfig.BootVariableRedirect = TRUE;
        FwRuntime->SetOverride (&FwrtConfig);
        DEBUG ((DEBUG_INFO, "OCVAR: Found FW NVRAM, forcing redirect %d\n", FwrtConfig.BootVariableRedirect));
      }
    } else {
      FwRuntime = NULL;
      DEBUG ((DEBUG_INFO, "OCVAR: Missing FW NVRAM, going on...\n"));
    }
  } else {
    FwRuntime = NULL;
  }

  DEBUG ((DEBUG_INFO, "OCVAR: Loading NVRAM from storage...\n"));

  Status = OcVariableRuntimeProtocol->LoadNvram (Storage, &Config->Nvram);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OCVAR: Emulated NVRAM load failed - %r\n", Status));
  }

  if (FwRuntime != NULL) {
    DEBUG ((DEBUG_INFO, "OCVAR: Restoring FW NVRAM...\n"));
    FwRuntime->SetOverride (NULL);
  }
}

VOID
EFIAPI
OcSaveLegacyNvram (
  VOID
  )
{
  EFI_STATUS                    Status;
  OC_VARIABLE_RUNTIME_PROTOCOL  *OcVariableRuntimeProtocol;

  Status = InternalLocateVariableRuntimeProtocol (&OcVariableRuntimeProtocol);
  if (EFI_ERROR (Status)) {
    return;
  }

  DEBUG ((DEBUG_INFO, "OCVAR: Saving NVRAM to storage...\n"));

  Status = OcVariableRuntimeProtocol->SaveNvram ();

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OCVAR: Emulated NVRAM save failed - %r\n", Status));
  }
}

VOID
EFIAPI
OcResetLegacyNvram (
  VOID
  )
{
  EFI_STATUS                    Status;
  OC_VARIABLE_RUNTIME_PROTOCOL  *OcVariableRuntimeProtocol;

  Status = InternalLocateVariableRuntimeProtocol (&OcVariableRuntimeProtocol);
  if (EFI_ERROR (Status)) {
    return;
  }

  DEBUG ((DEBUG_INFO, "OCVAR: Resetting NVRAM storage...\n"));

  Status = OcVariableRuntimeProtocol->ResetNvram ();

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OCVAR: Emulated NVRAM reset failed - %r\n", Status));
  }

  DirectResetCold ();
}

VOID
EFIAPI
OcSwitchToFallbackLegacyNvram (
  VOID
  )
{
  EFI_STATUS                    Status;
  OC_VARIABLE_RUNTIME_PROTOCOL  *OcVariableRuntimeProtocol;

  Status = InternalLocateVariableRuntimeProtocol (&OcVariableRuntimeProtocol);
  if (EFI_ERROR (Status)) {
    return;
  }

  DEBUG ((DEBUG_INFO, "OCVAR: Switching to fallback NVRAM storage...\n"));

  Status = OcVariableRuntimeProtocol->SwitchToFallback ();

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OCVAR: Emulated NVRAM switch to fallback failed - %r\n", Status));
  }
}

STATIC
VOID
OcDeleteNvram (
  IN OC_GLOBAL_CONFIG  *Config
  )
{
  EFI_STATUS   Status;
  UINT32       DeleteGuidIndex;
  UINT32       AddGuidIndex;
  UINT32       DeleteVariableIndex;
  UINT32       AddVariableIndex;
  CONST CHAR8  *AsciiVariableName;
  CHAR16       *UnicodeVariableName;
  GUID         VariableGuid;
  OC_ASSOC     *VariableMap;
  VOID         *CurrentValue;
  UINTN        CurrentValueSize;
  BOOLEAN      SameContents;

  for (DeleteGuidIndex = 0; DeleteGuidIndex < Config->Nvram.Delete.Count; ++DeleteGuidIndex) {
    Status = OcProcessVariableGuid (
               OC_BLOB_GET (Config->Nvram.Delete.Keys[DeleteGuidIndex]),
               &VariableGuid,
               NULL,
               NULL
               );

    if (EFI_ERROR (Status)) {
      continue;
    }

    //
    // When variable is set and non-volatile variable setting is used,
    // we do not want a variable to be constantly removed and added every reboot,
    // as it will negatively impact flash memory. In case the variable is already set
    // and has the same value we do not delete it.
    //
    for (AddGuidIndex = 0; AddGuidIndex < Config->Nvram.Add.Count; ++AddGuidIndex) {
      if (AsciiStrCmp (
            OC_BLOB_GET (Config->Nvram.Delete.Keys[DeleteGuidIndex]),
            OC_BLOB_GET (Config->Nvram.Add.Keys[AddGuidIndex])
            ) == 0)
      {
        break;
      }
    }

    for (DeleteVariableIndex = 0; DeleteVariableIndex < Config->Nvram.Delete.Values[DeleteGuidIndex]->Count; ++DeleteVariableIndex) {
      AsciiVariableName = OC_BLOB_GET (Config->Nvram.Delete.Values[DeleteGuidIndex]->Values[DeleteVariableIndex]);

      //
      // '#' is filtered in all keys, but for values we need to do it ourselves.
      //
      if (AsciiVariableName[0] == '#') {
        DEBUG ((DEBUG_INFO, "OCVAR: Variable skip deleting %a\n", AsciiVariableName));
        continue;
      }

      UnicodeVariableName = AsciiStrCopyToUnicode (AsciiVariableName, 0);

      if (UnicodeVariableName == NULL) {
        DEBUG ((DEBUG_WARN, "OCVAR: Failed to convert NVRAM variable name %a\n", AsciiVariableName));
        continue;
      }

      if (AddGuidIndex != Config->Nvram.Add.Count) {
        VariableMap = NULL;
        for (AddVariableIndex = 0; AddVariableIndex < Config->Nvram.Add.Values[AddGuidIndex]->Count; ++AddVariableIndex) {
          if (AsciiStrCmp (AsciiVariableName, OC_BLOB_GET (Config->Nvram.Add.Values[AddGuidIndex]->Keys[AddVariableIndex])) == 0) {
            VariableMap = Config->Nvram.Add.Values[AddGuidIndex];
            break;
          }
        }

        if (VariableMap != NULL) {
          Status = GetVariable2 (UnicodeVariableName, &VariableGuid, &CurrentValue, &CurrentValueSize);

          if (!EFI_ERROR (Status)) {
            SameContents = CurrentValueSize == VariableMap->Values[AddVariableIndex]->Size
                           && CompareMem (OC_BLOB_GET (VariableMap->Values[AddVariableIndex]), CurrentValue, CurrentValueSize) == 0;
            FreePool (CurrentValue);
          } else if ((Status == EFI_NOT_FOUND) && (VariableMap->Values[AddVariableIndex]->Size == 0)) {
            SameContents = TRUE;
          } else {
            SameContents = FALSE;
          }

          if (SameContents) {
            DEBUG ((DEBUG_INFO, "OCVAR: Not deleting NVRAM %g:%a, matches add\n", &VariableGuid, AsciiVariableName));
            FreePool (UnicodeVariableName);
            continue;
          }
        }
      }

      Status = gRT->SetVariable (UnicodeVariableName, &VariableGuid, 0, 0, 0);
      DEBUG ((
        EFI_ERROR (Status) && Status != EFI_NOT_FOUND ? DEBUG_WARN : DEBUG_INFO,
        "OCVAR: Deleting NVRAM %g:%a - %r\n",
        &VariableGuid,
        AsciiVariableName,
        Status
        ));

      FreePool (UnicodeVariableName);
    }
  }
}

STATIC
VOID
OcAddNvram (
  IN OC_GLOBAL_CONFIG  *Config
  )
{
  EFI_STATUS  Status;
  UINT32      GuidIndex;
  UINT32      VariableIndex;
  GUID        VariableGuid;
  OC_ASSOC    *VariableMap;

  for (GuidIndex = 0; GuidIndex < Config->Nvram.Add.Count; ++GuidIndex) {
    Status = OcProcessVariableGuid (
               OC_BLOB_GET (Config->Nvram.Add.Keys[GuidIndex]),
               &VariableGuid,
               NULL,
               NULL
               );

    if (EFI_ERROR (Status)) {
      continue;
    }

    VariableMap = Config->Nvram.Add.Values[GuidIndex];

    for (VariableIndex = 0; VariableIndex < VariableMap->Count; ++VariableIndex) {
      OcDirectSetNvramVariable (
        OC_BLOB_GET (VariableMap->Keys[VariableIndex]),
        &VariableGuid,
        Config->Nvram.WriteFlash ? OPEN_CORE_NVRAM_NV_ATTR : OPEN_CORE_NVRAM_ATTR,
        VariableMap->Values[VariableIndex]->Size,
        OC_BLOB_GET (VariableMap->Values[VariableIndex]),
        NULL,
        FALSE
        );
    }
  }
}

VOID
OcLoadNvramSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config
  )
{
  OcLoadLegacyNvram (Storage, Config);

  OcDeleteNvram (Config);

  OcAddNvram (Config);

  OcReportVersion (Config);
}
