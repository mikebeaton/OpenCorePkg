/** @file
  Copyright (C) 2019, vit9696. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "BootManagementInternal.h"

#include <Guid/AppleFile.h>
#include <Guid/AppleVariable.h>
#include <Guid/GlobalVariable.h>
#include <Guid/OcVariable.h>

#include <IndustryStandard/AppleCsrConfig.h>

#include <Protocol/AppleBootPolicy.h>
#include <Protocol/AppleKeyMapAggregator.h>
#include <Protocol/AppleBeepGen.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/OcAudio.h>
#include <Protocol/SimpleTextOut.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/OcConsoleLib.h>
#include <Library/OcCryptoLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/DevicePathLib.h>
#include <Library/OcGuardLib.h>
#include <Library/OcTimerLib.h>
#include <Library/OcTypingLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAppleKeyMapLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcDevicePathLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcMiscLib.h>
#include <Library/OcRtcLib.h>
#include <Library/OcStringLib.h>
#include <Library/OcVariableLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/ResetSystemLib.h>

STATIC
EFI_STATUS
RunShowMenu (
  IN  OC_BOOT_CONTEXT  *BootContext,
  OUT OC_BOOT_ENTRY    **ChosenBootEntry
  )
{
  EFI_STATUS     Status;
  OC_BOOT_ENTRY  **BootEntries;
  UINT32         EntryReason;

  if (  !BootContext->PickerContext->ApplePickerUnsupported
     && (BootContext->PickerContext->PickerMode == OcPickerModeApple))
  {
    Status = OcRunFirmwareApplication (&gAppleBootPickerFileGuid, TRUE);
    //
    // This should not return on success.
    //
    DEBUG ((DEBUG_INFO, "OCB: Apple BootPicker failed on error - %r, fallback to builtin\n", Status));
    BootContext->PickerContext->ApplePickerUnsupported = TRUE;
  }

  BootEntries = OcEnumerateEntries (BootContext);
  if (BootEntries == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // We are not allowed to have no default entry.
  // However, if default entry is a tool or a system entry, never autoboot it.
  //
  if (BootContext->DefaultEntry == NULL) {
    BootContext->DefaultEntry                  = BootEntries[0];
    BootContext->PickerContext->TimeoutSeconds = 0;
  }

  //
  // Ensure that picker entry reason is set as it can be read by boot.efi.
  //
  EntryReason = ApplePickerEntryReasonUnknown;
  gRT->SetVariable (
         APPLE_PICKER_ENTRY_REASON_VARIABLE_NAME,
         &gAppleVendorVariableGuid,
         EFI_VARIABLE_BOOTSERVICE_ACCESS,
         sizeof (EntryReason),
         &EntryReason
         );

  Status = OcInitHotKeys (BootContext->PickerContext);
  if (EFI_ERROR (Status)) {
    FreePool (BootEntries);
    return Status;
  }

  Status = BootContext->PickerContext->ShowMenu (
                                         BootContext,
                                         BootEntries,
                                         ChosenBootEntry
                                         );

  OcFreeHotKeys (BootContext->PickerContext);

  FreePool (BootEntries);

  return Status;
}

BOOLEAN
EFIAPI
OcVerifyPassword (
  IN CONST UINT8                 *Password,
  IN UINT32                      PasswordSize,
  IN CONST OC_PRIVILEGE_CONTEXT  *PrivilegeContext
  )
{
  BOOLEAN  Result;

  Result = OcVerifyPasswordSha512 (
             Password,
             PasswordSize,
             PrivilegeContext->Salt,
             PrivilegeContext->SaltSize,
             PrivilegeContext->Hash
             );

  return Result;
}

EFI_STATUS
InternalRunRequestPrivilege (
  IN OC_PICKER_CONTEXT   *PickerContext,
  IN OC_PRIVILEGE_LEVEL  Level
  )
{
  EFI_STATUS  Status;
  BOOLEAN     HotKeysAlreadyLive;

  if (  (PickerContext->PrivilegeContext == NULL)
     || (PickerContext->PrivilegeContext->CurrentLevel >= Level))
  {
    return EFI_SUCCESS;
  }

  HotKeysAlreadyLive = (PickerContext->HotKeyContext != NULL);

  if (!HotKeysAlreadyLive) {
    Status = OcInitHotKeys (PickerContext);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = PickerContext->RequestPrivilege (
                            PickerContext,
                            OcPrivilegeAuthorized
                            );
  if (!EFI_ERROR (Status)) {
    PickerContext->PrivilegeContext->CurrentLevel = Level;
  }

  if (!HotKeysAlreadyLive) {
    OcFreeHotKeys (PickerContext);
  }

  return Status;
}

EFI_STATUS
OcRunAppleBootPicker (
  IN OC_PICKER_CONTEXT *Context
  )
{
#if 0
  EFI_STATUS  Status;
  CHAR16      *BootOrderName;
  CHAR16      *BootNextName;
  EFI_GUID    *BootVariableGuid;
  UINTN       VariableSize;
  UINT32      VariableAttributes;
  UINT16      BootNext;
  UINT16      *BootOrder;

  if (Context->CustomBootGuid) {
    BootVariableGuid = &gOcVendorVariableGuid;
  } else {
    BootVariableGuid = &gEfiGlobalVariableGuid;
  }

  if (CompareGuid (BootVariableGuid, &gOcVendorVariableGuid)) {
    BootOrderName = OC_VENDOR_BOOT_ORDER_VARIABLE_NAME;
    BootNextName  = OC_VENDOR_BOOT_NEXT_VARIABLE_NAME;
  } else {
    BootOrderName = EFI_BOOT_ORDER_VARIABLE_NAME;
    BootNextName  = EFI_BOOT_NEXT_VARIABLE_NAME;
  }

  VariableSize = 0;
  Status       = gRT->GetVariable (
                        BootOrderName,
                        BootVariableGuid,
                        &VariableAttributes,
                        &VariableSize,
                        NULL
                        );

  if (Status == EFI_BUFFER_TOO_SMALL) {
    BootOrder = AllocatePool (VariableSize);
    if (BootOrder == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Status = gRT->GetVariable (
                    BootOrderName,
                    BootVariableGuid,
                    &VariableAttributes,
                    &VariableSize,
                    BootOrder
                    );
    if (EFI_ERROR (Status)) {
      FreePool (BootOrder);
      Status = EFI_UNSUPPORTED;
    }
  } else {
    if (!EFI_ERROR (Status)) {
      return EFI_NOT_FOUND;
    } else if (Status == EFI_NOT_FOUND) {
      BootOrder = AllocateZeroPool (sizeof (BootNext)); ///< create fake entry of Boot0000
    } else {
      return Status;
    }
  }

  Status = gRT->SetVariable (
                    BootNextName,
                    BootVariableGuid,
                    EFI_VARIABLE_BOOTSERVICE_ACCESS
                    | EFI_VARIABLE_RUNTIME_ACCESS
                    | EFI_VARIABLE_NON_VOLATILE,
                    sizeof (BootNext),
                    BootOrder
                    );

  FreePool (BootOrder);
#else
  EFI_STATUS  Status;

  OcForceReconnectAppleGop ();
  
  OcBlockDriverReconnection ();
  Status = OcRunFirmwareApplication (&gAppleBootPickerFileGuid, TRUE);
  OcUnblockDriverReconnection ();

  return Status;
#endif
}

EFI_STATUS
OcRunBootPicker (
  IN OC_PICKER_CONTEXT  *Context
  )
{
  EFI_STATUS                         Status;
  APPLE_KEY_MAP_AGGREGATOR_PROTOCOL  *KeyMap;
  OC_BOOT_CONTEXT                    *BootContext;
  OC_BOOT_ENTRY                      *Chosen;
  BOOLEAN                            SaidWelcome;
  OC_FIRMWARE_RUNTIME_PROTOCOL       *FwRuntime;
  BOOLEAN                            IsApplePickerSelection;

  SaidWelcome = FALSE;

  OcImageLoaderActivate ();

  KeyMap = OcAppleKeyMapInstallProtocols (FALSE);
  if (KeyMap == NULL) {
    DEBUG ((DEBUG_ERROR, "OCB: AppleKeyMap locate failure\n"));
    return EFI_NOT_FOUND;
  }

  //
  // This one is handled as is for Apple BootPicker for now.
  //
  if (Context->PickerCommand != OcPickerDefault) {
    Status = InternalRunRequestPrivilege (Context, OcPrivilegeAuthorized);
    if (EFI_ERROR (Status)) {
      if (Status != EFI_ABORTED) {
        ASSERT (FALSE);
        return Status;
      }

      Context->PickerCommand = OcPickerDefault;
    }
  }

#if defined(OC_TARGET_NOOPT)
  WaitForKeyPress (L"Mike...");
#endif
  IsApplePickerSelection = FALSE;

  if ((Context->PickerCommand == OcPickerShowPicker) && (Context->PickerMode == OcPickerModeApple)) {
    Status = OcRunAppleBootPicker (Context);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "OCB: Apple BootPicker failed - %r, fallback to builtin\n", Status));
      Context->ApplePickerUnsupported = TRUE;
    } else {
      IsApplePickerSelection = TRUE;
    }
  }

  if ((Context->PickerCommand != OcPickerShowPicker) && (Context->PickerCommand != OcPickerDefault)) {
    //
    // We cannot ignore auxiliary entries for all other modes.
    //
    Context->HideAuxiliary = FALSE;
  }

  while (TRUE) {
    //
    // Never show Apple Picker twice, re-scan for entries if we previously successfully showed it.
    //
    if (IsApplePickerSelection) {
      if (Context->PickerMode == OcPickerModeApple) {
        Context->PickerMode = OcPickerModeBuiltin;
      } else {
        IsApplePickerSelection  = FALSE;
        Context->BootOrder      = NULL;
        Context->BootOrderCount = 0;
      }
    }

    //
    // Turbo-boost scanning when bypassing picker.
    //
    if ((Context->PickerCommand == OcPickerDefault)
      || (Context->PickerCommand == OcPickerProtocolHotKey)
      || IsApplePickerSelection
      ) {
      BootContext = OcScanForDefaultBootEntry (Context, IsApplePickerSelection);
    } else {
      ASSERT (
        Context->PickerCommand == OcPickerShowPicker
             || Context->PickerCommand == OcPickerBootApple
             || Context->PickerCommand == OcPickerBootAppleRecovery
        );

      BootContext = OcScanForBootEntries (Context);
    }

    //
    // We have no entries at all or have auxiliary entries.
    // Fallback to showing menu in the latter case.
    //
    if (BootContext == NULL) {
      //
      // TODO: Because of this fallback code, failed protocol hotkey can access OcPickerShowPicker
      // mode even if this is denied by InternalRunRequestPrivilege above; is this OK?
      //
      if (Context->HideAuxiliary || (Context->PickerCommand == OcPickerProtocolHotKey) || IsApplePickerSelection) {
        Context->PickerCommand = OcPickerShowPicker;
        Context->HideAuxiliary = FALSE;
        if (IsApplePickerSelection) {
          DEBUG ((DEBUG_WARN, "OCB: Apple Picker returned no entry valid under OC, falling back to builtin\n"));
        } else {
          DEBUG ((DEBUG_INFO, "OCB: System has no boot entries, showing picker with auxiliary\n"));
        }
        continue;
      }

      DEBUG ((DEBUG_WARN, "OCB: System has no boot entries\n"));
      return EFI_NOT_FOUND;
    }

    if (Context->PickerCommand == OcPickerShowPicker && !IsApplePickerSelection) {
      DEBUG ((
        DEBUG_INFO,
        "OCB: Showing menu... %a\n",
        Context->PollAppleHotKeys ? "(polling hotkeys)" : ""
        ));

      if (!SaidWelcome) {
        OcPlayAudioFile (Context, OC_VOICE_OVER_AUDIO_FILE_WELCOME, OC_VOICE_OVER_AUDIO_BASE_TYPE_OPEN_CORE, FALSE);
        SaidWelcome = TRUE;
      }

      Status = RunShowMenu (BootContext, &Chosen);

      if (EFI_ERROR (Status) && (Status != EFI_ABORTED)) {
        if (BootContext->PickerContext->ShowMenu != OcShowSimpleBootMenu) {
          DEBUG ((DEBUG_WARN, "OCB: External interface ShowMenu failure, fallback to builtin - %r\n", Status));
          BootContext->PickerContext->ShowMenu         = OcShowSimpleBootMenu;
          BootContext->PickerContext->RequestPrivilege = OcShowSimplePasswordRequest;
          Status                                       = RunShowMenu (BootContext, &Chosen);
        }
      }

      if (EFI_ERROR (Status) && (Status != EFI_ABORTED)) {
        DEBUG ((DEBUG_ERROR, "OCB: ShowMenu failed - %r\n", Status));
        OcFreeBootContext (BootContext);
        return Status;
      }
    } else if (BootContext->DefaultEntry != NULL) {
      Chosen = BootContext->DefaultEntry;
      Status = EFI_SUCCESS;
    } else {
      //
      // This can only be failed macOS or macOS recovery boot.
      // We may actually not rescan here.
      //
      ASSERT (Context->PickerCommand == OcPickerBootApple || Context->PickerCommand == OcPickerBootAppleRecovery);
      DEBUG ((DEBUG_INFO, "OCB: System has no default boot entry, showing menu\n"));
      Context->PickerCommand = OcPickerShowPicker;
      OcFreeBootContext (BootContext);
      continue;
    }

    ASSERT (!EFI_ERROR (Status) || Status == EFI_ABORTED);

    Context->TimeoutSeconds = 0;

    if (!EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_INFO,
        "OCB: Should boot from %u. %s (T:%d|F:%d|G:%d|E:%d|DEF:%d)\n",
        Chosen->EntryIndex,
        Chosen->Name,
        Chosen->Type,
        Chosen->IsFolder,
        Chosen->IsGeneric,
        Chosen->IsExternal,
        Chosen->SetDefault
        ));

      if (Context->PickerCommand == OcPickerShowPicker) {
        ASSERT (Chosen->EntryIndex > 0);

        if (Chosen->SetDefault) {
          if (Context->PickerCommand == OcPickerShowPicker) {
            OcPlayAudioFile (Context, OC_VOICE_OVER_AUDIO_FILE_SELECTED, OC_VOICE_OVER_AUDIO_BASE_TYPE_OPEN_CORE, FALSE);
            OcPlayAudioFile (Context, OC_VOICE_OVER_AUDIO_FILE_DEFAULT, OC_VOICE_OVER_AUDIO_BASE_TYPE_OPEN_CORE, FALSE);
            OcPlayAudioEntry (Context, Chosen);
          }

          Status = OcSetDefaultBootEntry (Context, Chosen);
          DEBUG ((DEBUG_INFO, "OCB: Setting default - %r\n", Status));
        }

        //
        // Clear screen of previous console contents - e.g. from builtin picker,
        // log messages or previous console tool - before loading the entry.
        //
        if (Chosen->LaunchInText) {
          gST->ConOut->ClearScreen (gST->ConOut);
        }

        if (Context->ShowMenu == OcShowSimpleBootMenu) {
          gST->ConOut->TestString (gST->ConOut, OC_CONSOLE_MARK_UNCONTROLLED);
        }

        //
        // Voice chosen information.
        //
        OcPlayAudioFile (Context, OC_VOICE_OVER_AUDIO_FILE_LOADING, OC_VOICE_OVER_AUDIO_BASE_TYPE_OPEN_CORE, FALSE);
        Status = OcPlayAudioEntry (Context, Chosen);
        if (EFI_ERROR (Status)) {
          OcPlayAudioBeep (
            Context,
            OC_VOICE_OVER_SIGNALS_PASSWORD_OK,
            OC_VOICE_OVER_SIGNAL_NORMAL_MS,
            OC_VOICE_OVER_SILENCE_NORMAL_MS
            );
        }
      }

      FwRuntime = Chosen->FullNvramAccess ? OcDisableNvramProtection () : NULL;

      Status = OcLoadBootEntry (
                 Context,
                 Chosen,
                 gImageHandle
                 );

      OcRestoreNvramProtection (FwRuntime);

      //
      // Do not wait on successful return code.
      //
      if (EFI_ERROR (Status)) {
        OcPlayAudioFile (Context, OC_VOICE_OVER_AUDIO_FILE_EXECUTION_FAILURE, OC_VOICE_OVER_AUDIO_BASE_TYPE_OPEN_CORE, TRUE);
        gBS->Stall (SECONDS_TO_MICROSECONDS (3));
        //
        // Show picker on first failure.
        //
        Context->PickerCommand = OcPickerShowPicker;
      } else {
        OcPlayAudioFile (Context, OC_VOICE_OVER_AUDIO_FILE_EXECUTION_SUCCESSFUL, OC_VOICE_OVER_AUDIO_BASE_TYPE_OPEN_CORE, FALSE);
      }

      //
      // Ensure that we flush all pressed keys after the application.
      // This resolves the problem of application-pressed keys being used to control the menu.
      //
      OcKeyMapFlush (KeyMap, 0, TRUE);
    }

    OcFreeBootContext (BootContext);
  }
}

EFI_STATUS
OcRunFirmwareApplication (
  IN EFI_GUID  *ApplicationGuid,
  IN BOOLEAN   SetReason
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 NewHandle;
  EFI_DEVICE_PATH_PROTOCOL   *Dp;
  APPLE_PICKER_ENTRY_REASON  PickerEntryReason;

  DEBUG ((DEBUG_INFO, "OCB: run fw app attempting to find %g...\n", ApplicationGuid));

  Dp = OcCreateFvFileDevicePath (ApplicationGuid);
  if (Dp != NULL) {
    DEBUG ((DEBUG_INFO, "OCB: run fw app attempting to load %g...\n", ApplicationGuid));
    NewHandle = NULL;
    Status    = gBS->LoadImage (
                       FALSE,
                       gImageHandle,
                       Dp,
                       NULL,
                       0,
                       &NewHandle
                       );
    if (EFI_ERROR (Status)) {
      Status = EFI_INVALID_PARAMETER;
    }
  } else {
    Status = EFI_NOT_FOUND;
  }

  if (!EFI_ERROR (Status)) {
    if (SetReason) {
      PickerEntryReason = ApplePickerEntryReasonUnknown;
      Status            = gRT->SetVariable (
                                 APPLE_PICKER_ENTRY_REASON_VARIABLE_NAME,
                                 &gAppleVendorVariableGuid,
                                 EFI_VARIABLE_BOOTSERVICE_ACCESS,
                                 sizeof (PickerEntryReason),
                                 &PickerEntryReason
                                 );
    }

    DEBUG ((
      DEBUG_INFO,
      "OCB: run fw app attempting to start %g (%d) %r...\n",
      ApplicationGuid,
      SetReason,
      Status
      ));
    Status = gBS->StartImage (
                    NewHandle,
                    NULL,
                    NULL
                    );

    if (EFI_ERROR (Status)) {
      Status = EFI_UNSUPPORTED;
    }
  }

  return Status;
}
