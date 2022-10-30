/** @file
  Boot entry protocol implementation of Reset NVRAM boot picker entry.

  Copyright (c) 2022, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Guid/AppleVariable.h>

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/OcDirectResetLib.h>
#include <Library/OcMemoryLib.h>
#include <Library/OcVariableLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/OcBootEntry.h>

#define OC_MENU_RESET_NVRAM_ID     "reset_nvram"
#define OC_MENU_RESET_NVRAM_ENTRY  "Reset NVRAM"

STATIC BOOLEAN  mUseApple           = FALSE;
STATIC BOOLEAN  mPreserveBoot       = FALSE;
STATIC BOOLEAN  mDisableExternalGpu = FALSE;

STATIC EFI_EXIT_BOOT_SERVICES mOriginalExitBootServices;
STATIC EFI_GET_MEMORY_MAP     mOriginalGetMemoryMap;

STATIC
VOID
WaitForChime (
  IN OUT          OC_PICKER_CONTEXT  *Context
  )
{
  //
  // Allow chime to finish, if playing.
  //
  if (Context->OcAudio != NULL) {
    Context->OcAudio->StopPlayback (Context->OcAudio, TRUE);
  }
}

STATIC
EFI_STATUS
PerformReset (
  VOID
  )
{
  DirectResetCold ();
  return EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
AppleReset (
  VOID
  )
{
  UINT8  ResetNVRam = 1;

  //
  // Any size, any value for this variable will cause a reset on supported firmware.
  //
  gRT->SetVariable (
        APPLE_RESET_NVRAM_VARIABLE_NAME,
        &gAppleBootVariableGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
        sizeof (ResetNVRam),
        &ResetNVRam
        );

  return PerformReset ();
}

//
// Set gpu-power-prefs after faking exit to non-macOS OS, so that we have permissions to do so.
//
STATIC
EFI_STATUS
DisableExternalGpu (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *MemoryMap;
  UINTN                  MapKey;
  UINTN                  MemoryMapSize;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
  UINT32                 GpuPowerPrefs;
  UINTN                  DataSize;

  DataSize = sizeof (GpuPowerPrefs);
  Status = gRT->GetVariable (
    L"gpu-power-prefs",
    &gApplePersonalizationVariableGuid,
    NULL,
    &DataSize,
    &GpuPowerPrefs
    );

  if (!EFI_ERROR (Status) && GpuPowerPrefs == 1) {
    return PerformReset ();
  }

  GpuPowerPrefs = 1;

  Status = OcGetCurrentMemoryMapAlloc (
              &MemoryMapSize,
              &MemoryMap,
              &MapKey,
              &DescriptorSize,
              &DescriptorVersion,
              mOriginalGetMemoryMap,
              NULL
              );

  ///////////
  // OcSetFileData (
  //   NULL,
  //   L"status.bin",
  //   &Status,
  //   sizeof (Status)
  //   );
  ///////////

  if (EFI_ERROR (Status)) {
    MicroSecondDelay (10000);
    return PerformReset ();
  }

  Status = mOriginalExitBootServices (NULL, MapKey);

  if (EFI_ERROR (Status)) {
    MicroSecondDelay (20000);
    return PerformReset ();
  }

  Status = gRT->SetVariable (
    L"gpu-power-prefs",
    &gApplePersonalizationVariableGuid,
    EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
    DataSize,
    &GpuPowerPrefs
    );

  if (EFI_ERROR (Status)) {
    MicroSecondDelay (30000);
    return PerformReset ();
  }

  return PerformReset ();
}

STATIC
EFI_STATUS
SystemActionResetNvram (
  IN OUT          OC_PICKER_CONTEXT  *PickerContext
  )
{
  WaitForChime (PickerContext);

  if (mUseApple) {
    return AppleReset ();
  }

  if (!OcResetLegacyNvram ()) {
    OcResetNvram (mPreserveBoot);
  }

  if (mDisableExternalGpu) {
    //
    // Because of Apple NVRAM redirection, this can apply even when we are using
    // emulated NVRAM.
    //
    return DisableExternalGpu ();
  }

  return PerformReset ();
}

STATIC OC_PICKER_ENTRY  mResetNvramBootEntries[1] = {
  {
    .Id            = OC_MENU_RESET_NVRAM_ID,
    .Name          = OC_MENU_RESET_NVRAM_ENTRY,
    .Path          = NULL,
    .Arguments     = NULL,
    .Flavour       = OC_FLAVOUR_RESET_NVRAM,
    .Auxiliary     = TRUE,
    .Tool          = FALSE,
    .TextMode      = FALSE,
    .RealPath      = FALSE,
    .SystemAction  = SystemActionResetNvram,
    .AudioBasePath = OC_VOICE_OVER_AUDIO_FILE_RESET_NVRAM,
    .AudioBaseType = OC_VOICE_OVER_AUDIO_BASE_TYPE_OPEN_CORE
  }
};

STATIC
EFI_STATUS
EFIAPI
ResetNvramGetBootEntries (
  IN OUT          OC_PICKER_CONTEXT  *PickerContext,
  IN     CONST EFI_HANDLE            Device OPTIONAL,
  OUT       OC_PICKER_ENTRY          **Entries,
  OUT       UINTN                    *NumEntries
  )
{
  //
  // Custom entries only.
  //
  if (Device != NULL) {
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_INFO, "BEP: Reset NVRAM entry b/a/d=%u/%u/%u\n", mPreserveBoot, mUseApple, mDisableExternalGpu));

  *Entries    = mResetNvramBootEntries;
  *NumEntries = ARRAY_SIZE (mResetNvramBootEntries);

  return EFI_SUCCESS;
}

STATIC
CHAR8 *
EFIAPI
ResetNvramCheckHotKeys (
  IN OUT OC_PICKER_CONTEXT  *Context,
  IN UINTN                  NumKeys,
  IN APPLE_MODIFIER_MAP     Modifiers,
  IN APPLE_KEY_CODE         *Keys
  )
{
  BOOLEAN  HasCommand;
  BOOLEAN  HasOption;
  BOOLEAN  HasKeyP;
  BOOLEAN  HasKeyR;

  HasCommand = (Modifiers & (APPLE_MODIFIER_LEFT_COMMAND | APPLE_MODIFIER_RIGHT_COMMAND)) != 0;
  HasOption  = (Modifiers & (APPLE_MODIFIER_LEFT_OPTION  | APPLE_MODIFIER_RIGHT_OPTION)) != 0;
  HasKeyP    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyP);
  HasKeyR    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyR);

  if (HasOption && HasCommand && HasKeyP && HasKeyR) {
    DEBUG ((DEBUG_INFO, "BEP: CMD+OPT+P+R causes NVRAM reset\n"));
    return OC_MENU_RESET_NVRAM_ID;
  }

  return NULL;
}

STATIC
OC_BOOT_ENTRY_PROTOCOL
  mResetNvramBootEntryProtocol = {
  OC_BOOT_ENTRY_PROTOCOL_REVISION,
  ResetNvramGetBootEntries,
  NULL,
  ResetNvramCheckHotKeys
};

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  OC_FLEX_ARRAY              *ParsedLoadOptions;

  mOriginalExitBootServices = gBS->ExitBootServices;
  mOriginalGetMemoryMap     = gBS->GetMemoryMap;

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = OcParseLoadOptions (LoadedImage, &ParsedLoadOptions);
  if (!EFI_ERROR (Status)) {
    mPreserveBoot       = OcHasParsedVar (ParsedLoadOptions, L"--preserve-boot", OcStringFormatUnicode);
    mUseApple           = OcHasParsedVar (ParsedLoadOptions, L"--apple", OcStringFormatUnicode);
    mDisableExternalGpu = OcHasParsedVar (ParsedLoadOptions, L"--disable-ext-gpu", OcStringFormatUnicode);

    OcFlexArrayFree (&ParsedLoadOptions);
  } else {
    ASSERT (ParsedLoadOptions == NULL);

    if (Status != EFI_NOT_FOUND) {
      return Status;
    }
  }

  if (mUseApple && mPreserveBoot) {
    DEBUG ((DEBUG_WARN, "BEP: ResetNvram %s is ignored due to %s!\n", L"--preserve-boot", L"--apple"));
  }

  if (mUseApple && mDisableExternalGpu) {
    DEBUG ((DEBUG_WARN, "BEP: ResetNvram %s is ignored due to %s!\n", L"--disable-ext-gpu", L"--apple"));
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gOcBootEntryProtocolGuid,
                  &mResetNvramBootEntryProtocol,
                  NULL
                  );
  return Status;
}
