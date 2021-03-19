/** @file
  Copyright (C) 2019, vit9696. All rights reserved.<BR>
  Copyright (C) 2021, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "BootManagementInternal.h"

#include <Guid/AppleVariable.h>
#include <IndustryStandard/AppleCsrConfig.h>
#include <Protocol/AppleKeyMapAggregator.h>

#include <Library/BaseLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/OcTimerLib.h>
#include <Library/OcAppleKeyMapLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcConfigurationLib.h>
#include <Library/OcMiscLib.h>
#include <Library/OcTemplateLib.h>
#include <Library/OcTypingLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

//
// Get hotkeys pressed at load
//
VOID
OcLoadPickerHotKeys (
  IN OUT OC_PICKER_CONTEXT  *Context
  )
{
  EFI_STATUS                         Status;
  APPLE_KEY_MAP_AGGREGATOR_PROTOCOL  *KeyMap;

  UINTN                              NumKeys;
  APPLE_MODIFIER_MAP                 Modifiers;
  APPLE_KEY_CODE                     Keys[OC_KEY_MAP_DEFAULT_SIZE];

  BOOLEAN                            HasCommand;
  BOOLEAN                            HasEscape;
  BOOLEAN                            HasOption;
  BOOLEAN                            HasKeyP;
  BOOLEAN                            HasKeyR;
  BOOLEAN                            HasKeyX;

  if (Context->TakeoffDelay > 0) {
    gBS->Stall (Context->TakeoffDelay);
  }

  KeyMap = OcGetProtocol (&gAppleKeyMapAggregatorProtocolGuid, DEBUG_ERROR, "OcLoadPickerHotKeys", "AppleKeyMapAggregator");

  NumKeys = ARRAY_SIZE (Keys);
  Status = KeyMap->GetKeyStrokes (
    KeyMap,
    &Modifiers,
    &NumKeys,
    Keys
    );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OCHK: GetKeyStrokes - %r\n", Status));
    return;
  }

  //
  // I do not like this code a little, as it is prone to race conditions during key presses.
  // For the good false positives are not too critical here, and in reality users are not that fast.
  //
  // Reference key list:
  // https://support.apple.com/HT201255
  // https://support.apple.com/HT204904
  //
  // We are slightly more permissive than AppleBds, as we permit combining keys.
  //

  HasCommand = (Modifiers & (APPLE_MODIFIER_LEFT_COMMAND | APPLE_MODIFIER_RIGHT_COMMAND)) != 0;
  HasOption  = (Modifiers & (APPLE_MODIFIER_LEFT_OPTION  | APPLE_MODIFIER_RIGHT_OPTION)) != 0;
  HasEscape  = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyEscape);
  HasKeyP    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyP);
  HasKeyR    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyR);
  HasKeyX    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyX);

  if (HasOption && HasCommand && HasKeyP && HasKeyR) {
    DEBUG ((DEBUG_INFO, "OCHK: CMD+OPT+P+R causes NVRAM reset\n"));
    Context->PickerCommand = OcPickerResetNvram;
  } else if (HasCommand && HasKeyR) {
    DEBUG ((DEBUG_INFO, "OCHK: CMD+R causes recovery to boot\n"));
    Context->PickerCommand = OcPickerBootAppleRecovery;
  } else if (HasKeyX) {
    DEBUG ((DEBUG_INFO, "OCHK: X causes macOS to boot\n"));
    Context->PickerCommand = OcPickerBootApple;
  } else if (HasOption) {
    DEBUG ((DEBUG_INFO, "OCHK: OPT causes picker to show\n"));
    Context->PickerCommand = OcPickerShowPicker;
  } else if (HasEscape) {
    DEBUG ((DEBUG_INFO, "OCHK: ESC causes picker to show as OC extension\n"));
    Context->PickerCommand = OcPickerShowPicker;
  } else {
    //
    // In addition to these overrides we always have ShowPicker = YES in config.
    // The following keys are not implemented:
    // C - CD/DVD boot, legacy that is gone now.
    // D - Diagnostics, could implement dumping stuff here in some future,
    //     but we will need to store the data before handling the key.
    //     Should also be DEBUG only for security reasons.
    // N - Network boot, simply not supported (and bad for security).
    // T - Target disk mode, simply not supported (and bad for security).
    //
  }
}

//
// Initialise picker keyboard handling.
//
VOID
OcInitHotKeys (
  IN OUT OC_PICKER_CONTEXT  *Context
  )
{
  APPLE_KEY_MAP_AGGREGATOR_PROTOCOL  *KeyMap;
  EFI_STATUS                         Status;

  DEBUG ((DEBUG_INFO, "OCHK: InitHotKeys\n"));

  //
  // No kb debug unless initialiased on settings flag by a given picker itself.
  //
  Context->KbDebug = NULL;

  KeyMap = OcGetProtocol (&gAppleKeyMapAggregatorProtocolGuid, DEBUG_ERROR, "OcInitHotKeys", "AppleKeyMapAggregator");

  //
  // Non-repeating keys e.g. ESC and SPACE.
  //
  Status = OcInitKeyRepeatContext (
    &Context->DoNotRepeatContext,
    KeyMap,
    OC_HELD_KEYS_DEFAULT_SIZE,
    0,
    0,
    TRUE
  );
  
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OCHK: Init non-repeating context - %r\n", Status));
  }

  //
  // Typing handler, for most keys.
  //
  Status = OcRegisterTypingHandler(&Context->TypingContext);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OCHK: Register typing handler - %r\n", Status));
  }

  //
  // NB Raw AKMA is also still used for HotKeys, since we really do need
  // three different types of keys response for fluent UI behaviour.
  //
}

INTN
EFIAPI
OcGetAppleKeyIndex (
  IN OUT OC_PICKER_CONTEXT                  *Context,
  IN     APPLE_KEY_MAP_AGGREGATOR_PROTOCOL  *KeyMap,
     OUT BOOLEAN                            *SetDefault  OPTIONAL
  )
{
  EFI_STATUS                         Status;
  APPLE_KEY_CODE                     KeyCode;

  UINTN                              NumKeys;
  APPLE_MODIFIER_MAP                 Modifiers;
  APPLE_KEY_CODE                     Key;
  APPLE_KEY_CODE                     *Keys;
  UINTN                              NumKeysUp;
  UINTN                              NumKeysDoNotRepeat;
  APPLE_KEY_CODE                     KeysDoNotRepeat[OC_KEY_MAP_DEFAULT_SIZE];

  UINTN                              AkmaNumKeys;
  APPLE_MODIFIER_MAP                 AkmaModifiers;
  APPLE_KEY_CODE                     AkmaKeys[OC_KEY_MAP_DEFAULT_SIZE];

  BOOLEAN                            HasCommand;
  BOOLEAN                            HasShift;
  BOOLEAN                            HasKeyC;
  BOOLEAN                            HasKeyK;
  BOOLEAN                            HasKeyS;
  BOOLEAN                            HasKeyV;
  BOOLEAN                            HasKeyMinus;
  BOOLEAN                            WantsZeroSlide;
  UINT32                             CsrActiveConfig;
  UINTN                              CsrActiveConfigSize;

  if (SetDefault != NULL) {
    *SetDefault = 0;
  }

  //
  // AKMA hotkeys
  //
  AkmaNumKeys         = ARRAY_SIZE (AkmaKeys);
  Status = KeyMap->GetKeyStrokes (
    KeyMap,
    &AkmaModifiers,
    &AkmaNumKeys,
    AkmaKeys
    );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OCHK: AKMA GetKeyStrokes - %r\n", Status));
    return OC_INPUT_INVALID;
  }

  //
  // Apple Event typing
  //
  Keys                = &Key;
  OcGetNextKeystroke(Context->TypingContext, &Modifiers, Keys);
  if (Key == 0) {
    NumKeys = 0;
  }
  else {
    NumKeys = 1;
  }

  //
  // Non-repeating keys
  //
  NumKeysUp           = 0;
  NumKeysDoNotRepeat  = ARRAY_SIZE (KeysDoNotRepeat);
  Status = OcGetUpDownKeys (
    Context->DoNotRepeatContext,
    &Modifiers,
    &NumKeysUp, NULL,
    &NumKeysDoNotRepeat, KeysDoNotRepeat,
    0ULL // time not needed for non-repeat keys
    );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OCHK: GetUpDownKeys for DoNotRepeatContext - %r\n", Status));
    return OC_INPUT_INVALID;
  }


  DEBUG_CODE_BEGIN();
  if (Context->KbDebug != NULL) {
    Context->KbDebug->Show (NumKeys, AkmaNumKeys, Modifiers);
  }
  DEBUG_CODE_END();

  //
  // Handle key combinations.
  //
  if (Context->PollAppleHotKeys) {
    HasCommand = (AkmaModifiers & (APPLE_MODIFIER_LEFT_COMMAND | APPLE_MODIFIER_RIGHT_COMMAND)) != 0;
    HasShift   = (AkmaModifiers & (APPLE_MODIFIER_LEFT_SHIFT | APPLE_MODIFIER_RIGHT_SHIFT)) != 0;
    HasKeyC    = OcKeyMapHasKey (AkmaKeys, AkmaNumKeys, AppleHidUsbKbUsageKeyC);
    HasKeyK    = OcKeyMapHasKey (AkmaKeys, AkmaNumKeys, AppleHidUsbKbUsageKeyK);
    HasKeyS    = OcKeyMapHasKey (AkmaKeys, AkmaNumKeys, AppleHidUsbKbUsageKeyS);
    HasKeyV    = OcKeyMapHasKey (AkmaKeys, AkmaNumKeys, AppleHidUsbKbUsageKeyV);
    //
    // Checking for PAD minus is our extension to support more keyboards.
    //
    HasKeyMinus = OcKeyMapHasKey (AkmaKeys, AkmaNumKeys, AppleHidUsbKbUsageKeyMinus)
      || OcKeyMapHasKey (AkmaKeys, AkmaNumKeys, AppleHidUsbKbUsageKeyPadMinus);

    //
    // Shift is always valid and enables Safe Mode.
    //
    if (HasShift) {
      if (OcGetArgumentFromCmd (Context->AppleBootArgs, "-x", L_STR_LEN ("-x"), NULL) == NULL) {
        DEBUG ((DEBUG_INFO, "OCHK: Shift means -x\n"));
        OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "-x", L_STR_LEN ("-x"));
      }
      return OC_INPUT_INTERNAL;
    }

    //
    // CMD+V is always valid and enables Verbose Mode.
    //
    if (HasCommand && HasKeyV) {
      if (OcGetArgumentFromCmd (Context->AppleBootArgs, "-v", L_STR_LEN ("-v"), NULL) == NULL) {
        DEBUG ((DEBUG_INFO, "OCHK: CMD+V means -v\n"));
        OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "-v", L_STR_LEN ("-v"));
      }
      return OC_INPUT_INTERNAL;
    }

    //
    // CMD+C+MINUS is always valid and disables compatibility check.
    //
    if (HasCommand && HasKeyC && HasKeyMinus) {
      if (OcGetArgumentFromCmd (Context->AppleBootArgs, "-no_compat_check", L_STR_LEN ("-no_compat_check"), NULL) == NULL) {
        DEBUG ((DEBUG_INFO, "OCHK: CMD+C+MINUS means -no_compat_check\n"));
        OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "-no_compat_check", L_STR_LEN ("-no_compat_check"));
      }
      return OC_INPUT_INTERNAL;
    }

    //
    // CMD+K is always valid for new macOS and means force boot to release kernel.
    //
    if (HasCommand && HasKeyK) {
      if (AsciiStrStr (Context->AppleBootArgs, "kcsuffix=release") == NULL) {
        DEBUG ((DEBUG_INFO, "OCHK: CMD+K means kcsuffix=release\n"));
        OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "kcsuffix=release", L_STR_LEN ("kcsuffix=release"));
      }
      return OC_INPUT_INTERNAL;
    }

    //
    // boot.efi also checks for CMD+X, but I have no idea what it is for.
    //

    //
    // boot.efi requires unrestricted NVRAM just for CMD+S+MINUS, and CMD+S
    // does not work at all on T2 macs. For CMD+S we simulate T2 behaviour with
    // DisableSingleUser Booter quirk if necessary.
    // Ref: https://support.apple.com/HT201573
    //
    if (HasCommand && HasKeyS) {
      WantsZeroSlide = HasKeyMinus;

      if (WantsZeroSlide) {
        CsrActiveConfig     = 0;
        CsrActiveConfigSize = sizeof (CsrActiveConfig);
        Status = gRT->GetVariable (
          L"csr-active-config",
          &gAppleBootVariableGuid,
          NULL,
          &CsrActiveConfigSize,
          &CsrActiveConfig
          );
        //
        // FIXME: CMD+S+Minus behaves as CMD+S when "slide=0" is not supported
        //        by the SIP configuration. This might be an oversight, but is
        //        consistent with the boot.efi implementation.
        //
        WantsZeroSlide = !EFI_ERROR (Status) && (CsrActiveConfig & CSR_ALLOW_UNRESTRICTED_NVRAM) != 0;
      }

      if (WantsZeroSlide) {
        if (AsciiStrStr (Context->AppleBootArgs, "slide=0") == NULL) {
          DEBUG ((DEBUG_INFO, "OCHK: CMD+S+MINUS means slide=0\n"));
          OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "slide=0", L_STR_LEN ("slide=0"));
        }
      } else if (OcGetArgumentFromCmd (Context->AppleBootArgs, "-s", L_STR_LEN ("-s"), NULL) == NULL) {
        DEBUG ((DEBUG_INFO, "OCHK: CMD+S means -s\n"));
        OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "-s", L_STR_LEN ("-s"));
      }
      return OC_INPUT_INTERNAL;
    }
  }

  //
  // Handle VoiceOver - non-repeating.
  //
  if ((Modifiers & (APPLE_MODIFIER_LEFT_COMMAND | APPLE_MODIFIER_RIGHT_COMMAND)) != 0
    && OcKeyMapHasKey (KeysDoNotRepeat, NumKeysDoNotRepeat, AppleHidUsbKbUsageKeyF5)) {
    return OC_INPUT_VOICE_OVER;
  }

  //
  // Handle reload menu - non-repeating.
  //
  if (OcKeyMapHasKey (KeysDoNotRepeat, NumKeysDoNotRepeat, AppleHidUsbKbUsageKeyEscape)
   || OcKeyMapHasKey (KeysDoNotRepeat, NumKeysDoNotRepeat, AppleHidUsbKbUsageKeyZero)) {
    return OC_INPUT_ABORTED;
  }

  //
  // Handle show or toggle auxiliary - non-repeating.
  //
  if (OcKeyMapHasKey (KeysDoNotRepeat, NumKeysDoNotRepeat, AppleHidUsbKbUsageKeySpaceBar)) {
    return OC_INPUT_MORE;
  }

  //
  // Default update is desired for Ctrl+Index and Ctrl+Enter.
  //
  if (SetDefault != NULL
    && Modifiers != 0
    && (Modifiers & ~(APPLE_MODIFIER_LEFT_CONTROL | APPLE_MODIFIER_RIGHT_CONTROL)) == 0) {
      *SetDefault = TRUE;
  }

  //
  // Check exact match on index strokes.
  //
  if ((Modifiers == 0 || (SetDefault != NULL && *SetDefault)) && NumKeys == 1) {
    if (Keys[0] == AppleHidUsbKbUsageKeyEnter
      || Keys[0] == AppleHidUsbKbUsageKeyReturn
      || Keys[0] == AppleHidUsbKbUsageKeyPadEnter) {
      return OC_INPUT_CONTINUE;
    }

    if (Keys[0] == AppleHidUsbKbUsageKeyUpArrow) {
      return OC_INPUT_UP;
    }

    if (Keys[0] == AppleHidUsbKbUsageKeyDownArrow) {
      return OC_INPUT_DOWN;
    }

    if (Keys[0] == AppleHidUsbKbUsageKeyLeftArrow) {
      return OC_INPUT_LEFT;
    }

    if (Keys[0] == AppleHidUsbKbUsageKeyRightArrow) {
      return OC_INPUT_RIGHT;
    }

    if (Keys[0] == AppleHidUsbKbUsageKeyPgUp
      || Keys[0] == AppleHidUsbKbUsageKeyHome) {
      return OC_INPUT_TOP;
    }

    if (Keys[0] == AppleHidUsbKbUsageKeyPgDn
      || Keys[0] == AppleHidUsbKbUsageKeyEnd) {
      return OC_INPUT_BOTTOM;
    }

    STATIC_ASSERT (AppleHidUsbKbUsageKeyF1 + 11 == AppleHidUsbKbUsageKeyF12, "Unexpected encoding");
    if (Keys[0] >= AppleHidUsbKbUsageKeyF1 && Keys[0] <= AppleHidUsbKbUsageKeyF12) {
      return OC_INPUT_FUNCTIONAL (Keys[0] - AppleHidUsbKbUsageKeyF1 + 1);
    }

    STATIC_ASSERT (AppleHidUsbKbUsageKeyF13 + 11 == AppleHidUsbKbUsageKeyF24, "Unexpected encoding");
    if (Keys[0] >= AppleHidUsbKbUsageKeyF13 && Keys[0] <= AppleHidUsbKbUsageKeyF24) {
      return OC_INPUT_FUNCTIONAL (Keys[0] - AppleHidUsbKbUsageKeyF13 + 13);
    }

    STATIC_ASSERT (AppleHidUsbKbUsageKeyOne + 8 == AppleHidUsbKbUsageKeyNine, "Unexpected encoding");
    for (KeyCode = AppleHidUsbKbUsageKeyOne; KeyCode <= AppleHidUsbKbUsageKeyNine; ++KeyCode) {
      if (OcKeyMapHasKey (Keys, NumKeys, KeyCode)) {
        return (INTN) (KeyCode - AppleHidUsbKbUsageKeyOne);
      }
    }

    STATIC_ASSERT (AppleHidUsbKbUsageKeyA + 25 == AppleHidUsbKbUsageKeyZ, "Unexpected encoding");
    for (KeyCode = AppleHidUsbKbUsageKeyA; KeyCode <= AppleHidUsbKbUsageKeyZ; ++KeyCode) {
      if (OcKeyMapHasKey (Keys, NumKeys, KeyCode)) {
        return (INTN) (KeyCode - AppleHidUsbKbUsageKeyA + 9);
      }
    }
  }

  if (NumKeys > 0) {
    return OC_INPUT_INVALID;
  }

  return OC_INPUT_TIMEOUT;
}

UINT64
OcWaitForAppleKeyIndexGetEndTime(
  IN UINTN    Timeout
  )
{
  if (Timeout == 0) {
    return 0ULL;
  }

  return GetTimeInNanoSecond (GetPerformanceCounter ()) + Timeout * 1000000u;
}

INTN
OcWaitForAppleKeyIndex (
  IN OUT OC_PICKER_CONTEXT                  *Context,
  IN     APPLE_KEY_MAP_AGGREGATOR_PROTOCOL  *KeyMap,
  IN     UINT64                             EndTime,
  IN OUT BOOLEAN                            *SetDefault  OPTIONAL
  )
{
  INTN                               ResultingKey;
  UINT64                             CurrTime;
  BOOLEAN                            OldSetDefault;

  UINT64                             LoopDelayStart = 0;

  //
  // These hotkeys are normally parsed by boot.efi, and they work just fine
  // when ShowPicker is disabled. On some BSPs, however, they may fail badly
  // when ShowPicker is enabled, and for this reason we support these hotkeys
  // within picker itself.
  //

  if (SetDefault != NULL) {
    OldSetDefault = *SetDefault;
    *SetDefault = 0;
  }

  while (TRUE) {
    ResultingKey = OcGetAppleKeyIndex (Context, KeyMap, SetDefault);

    CurrTime    = GetTimeInNanoSecond (GetPerformanceCounter ());  

    //
    // Requested for another iteration, handled Apple hotkey.
    //
    if (ResultingKey == OC_INPUT_INTERNAL) {
      continue;
    }

    //
    // Abort the timeout when unrecognised keys are pressed.
    //
    if (EndTime != 0 && ResultingKey == OC_INPUT_INVALID) {
      break;
    }

    //
    // Found key, return it.
    //
    if (ResultingKey != OC_INPUT_INVALID && ResultingKey != OC_INPUT_TIMEOUT) {
      break;
    }

    //
    // Return modifiers if they change, so we can optionally update UI
    //
    if (SetDefault != NULL && *SetDefault != OldSetDefault) {
      ResultingKey = OC_INPUT_MODIFIERS_ONLY;
      break;
    }

    if (EndTime != 0 && CurrTime != 0 && CurrTime >= EndTime) {
      ResultingKey = OC_INPUT_TIMEOUT;
      break;
    }

    DEBUG_CODE_BEGIN();
    LoopDelayStart = AsmReadTsc();
    DEBUG_CODE_END();

    MicroSecondDelay (OC_MINIMAL_CPU_DELAY);

    DEBUG_CODE_BEGIN();
    if (Context->KbDebug != NULL) {
      Context->KbDebug->InstrumentLoopDelay (LoopDelayStart, AsmReadTsc());
    }
    DEBUG_CODE_END();
  }

  return ResultingKey;
}
