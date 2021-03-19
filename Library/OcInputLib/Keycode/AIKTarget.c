/** @file
  Key consumer

Copyright (c) 2018, vit9696. All rights reserved.<BR>
Copyright (c) 2021, Mike Beaton. All rights reserved.<BR>
SPDX-License-Identifier: BSD-3-Clause
**/

#include "AIKTarget.h"
#include "AIKTranslate.h"

#include <Base.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS
AIKTargetInstall (
  IN OUT AIK_TARGET  *Target,
  IN     UINT8       KeyForgotThreshold
  )
{
  EFI_STATUS  Status;

  Status = EFI_SUCCESS;

  Target->KeyForgotThreshold = KeyForgotThreshold;

  if (Target->KeyMapDb == NULL) {
    Status = gBS->LocateProtocol (&gAppleKeyMapDatabaseProtocolGuid, NULL, (VOID **) &Target->KeyMapDb);

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "AppleKeyMapDatabaseProtocol is unavailable - %r\n", Status));
      return EFI_NOT_FOUND;
    }

    Status = Target->KeyMapDb->CreateKeyStrokesBuffer (
      Target->KeyMapDb, AIK_TARGET_BUFFER_SIZE, &Target->KeyMapDbIndex
      );

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "CreateKeyStrokesBuffer failed - %r\n", Status));
      Target->KeyMapDb = NULL;
    }
  }

  return Status;
}

VOID
AIKTargetUninstall (
  IN OUT AIK_TARGET  *Target
  )
{
  if (Target->KeyMapDb != NULL) {
    Target->KeyMapDb->RemoveKeyStrokesBuffer (Target->KeyMapDb, Target->KeyMapDbIndex);
    Target->KeyMapDb = NULL;
  }

  Target->NumberOfKeys = 0;
  Target->Modifiers = 0;
  ZeroMem (Target->ModifierCounters, sizeof (Target->ModifierCounters));
  ZeroMem (Target->Keys, sizeof (Target->Keys));
  ZeroMem (Target->KeyCounters, sizeof (Target->KeyCounters));
}

UINT64
AIKTargetRefresh (
  IN OUT AIK_TARGET  *Target
  )
{
  APPLE_MODIFIER_MAP  Mask;
  UINTN               Index;
  UINTN               Left;

  Target->Counter++;

  for (Index = 0; Index < Target->NumberOfKeys; Index++) {
    //
    // We reported this key Target->KeyForgetThreshold times, time to say goodbye.
    //
    if (Target->KeyCounters[Index] + Target->KeyForgotThreshold <= Target->Counter) {
      Left = Target->NumberOfKeys - (Index + 1);
      if (Left > 0) {
        CopyMem (
          &Target->KeyCounters[Index],
          &Target->KeyCounters[Index+1],
          sizeof (Target->KeyCounters[0]) * Left);
        CopyMem (
          &Target->Keys[Index],
          &Target->Keys[Index+1],
          sizeof (Target->Keys[0]) * Left);
      }
      Target->NumberOfKeys--;
    }
  }

  //
  // Smooth modifiers in same way as keys, as some hardware needs it.
  //
  for (Index = 0, Mask = BIT0; Index <= APPLE_MAX_USED_MODIFIER_BIT; Index++, Mask <<= 1) {
    if ((Target->Modifiers & Mask) != 0) {
      //
      // We last saw this modifier Target->KeyForgetThreshold times ago, time to say goodbye.
      //
      if (Target->ModifierCounters[Index] + Target->KeyForgotThreshold <= Target->Counter) {
        Target->Modifiers &= ~Mask;
      }
    }
  }

  return Target->Counter;
}

VOID
AIKTargetWriteEntry (
  IN OUT AIK_TARGET        *Target,
  IN     AMI_EFI_KEY_DATA  *KeyData
  )
{
  APPLE_MODIFIER_MAP  Modifiers;
  APPLE_MODIFIER_MAP  Mask;
  APPLE_KEY_CODE      Key;
  UINTN               Index;
  UINTN               InsertIndex;
  UINT64              OldestCounter;

  AIKTranslate (KeyData, &Modifiers, &Key);

  //
  // Add smoothing counters for modifiers too - some hardware reports them as fast repeating keys.
  //
  for (Index = 0, Mask = BIT0; Index <= APPLE_MAX_USED_MODIFIER_BIT; Index++, Mask <<= 1) {
    if ((Modifiers & Mask) != 0) {
      if ((Target->Modifiers & Mask) == 0) {
        Target->Modifiers |= Mask;
      }
      //
      // Add or update modifier
      //
      Target->ModifierCounters[Index] = Target->Counter;
    }
  }

  if (Key == UsbHidUndefined) {
    //
    // This is a modifier or an unsupported key.
    //
    return;
  }

  for (Index = 0; Index < Target->NumberOfKeys; Index++) {
    if (Target->Keys[Index] == Key) {
      //
      // This key was added previously, just update its counter.
      //
      Target->KeyCounters[Index] = Target->Counter;
      return;
    }
  }

  InsertIndex = Target->NumberOfKeys;

  //
  // This should not happen, but we have no room, replace the oldest key.
  //
  if (InsertIndex == AIK_TARGET_BUFFER_SIZE) {
    InsertIndex   = 0;
    OldestCounter = Target->KeyCounters[InsertIndex];
    for (Index = 1; Index < Target->NumberOfKeys; Index++) {
      if (OldestCounter > Target->KeyCounters[Index]) {
        OldestCounter = Target->KeyCounters[Index];
        InsertIndex   = Index;
      }
    }
    Target->NumberOfKeys--;
  }

  //
  // Insert the new key
  //
  Target->Keys[InsertIndex] = Key;
  Target->KeyCounters[InsertIndex] = Target->Counter;
  Target->NumberOfKeys++;
}

VOID
AIKTargetSubmit (
  IN OUT AIK_TARGET  *Target
  )
{
  EFI_STATUS  Status;

  if (Target->KeyMapDb != NULL) {
    Status = Target->KeyMapDb->SetKeyStrokeBufferKeys (
      Target->KeyMapDb,
      Target->KeyMapDbIndex,
      Target->Modifiers,
      Target->NumberOfKeys,
      Target->Keys
      );
  } else {
    Status = EFI_NOT_FOUND;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "Failed to submit keys to AppleMapDb - %r", Status));
  }
}
