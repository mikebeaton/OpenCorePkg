/** @file
  Probe GRUB menu entries.

  Copyright (c) 2021, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "LinuxBootInternal.h"

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcFlexArrayLib.h>
#include <Library/OcStringLib.h>
#include <Library/PrintLib.h>

#include <Protocol/OcBootEntry.h>

//
// No leading slash so can be relative to root or
// additional scan dir.
//
#define GRUB_GRUB_CFG         L"grub\\grub.cfg"

STATIC
EFI_STATUS
ProbeGrubAtDirectory (
  IN   EFI_FILE_PROTOCOL        *RootDirectory,
  IN   CHAR16                   *DirName,
  OUT  OC_PICKER_ENTRY          **Entries,
  OUT  UINTN                    *NumEntries
  )
{
  EFI_STATUS                      Status;
  CHAR8                           *GrubCfg;

  //
  // Only probe GRUB menu entries if grub/grub.cfg exists.
  // (grub2/grub.cfg seems to only be used in blspec-style distros, so only
  // add probing that if we ever actually see it used this way.)
  //
  GrubCfg   = OcReadFileFromDirectory (RootDirectory, GRUB_GRUB_CFG, NULL, 0);
  if (GrubCfg == NULL) {
    DEBUG ((DEBUG_INFO, "LNX: %s not found\n", GRUB_GRUB_CFG));
    return EFI_NOT_FOUND;
  }
  
  Status = InternalInitGrubVars ();
  if (!EFI_ERROR (Status)) {
    if (!EFI_ERROR (Status)) {
      DEBUG (((gLinuxBootFlags & LINUX_BOOT_LOG_VERBOSE) == 0 ? DEBUG_VERBOSE : DEBUG_INFO,
        "LNX: Reading %s\n", GRUB_GRUB_CFG));
      Status = InternalProcessGrubCfg (GrubCfg, FALSE); //TRUE);
    }
  }

  InternalFreeGrubVars ();

  FreePool (GrubCfg);

  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
DoProbeGrub (
  IN   EFI_FILE_PROTOCOL        *Directory,
  IN   CHAR16                   *DirName,
  OUT  OC_PICKER_ENTRY          **Entries,
  OUT  UINTN                    *NumEntries
  )
{
  EFI_STATUS                      Status;

  Status = ProbeGrubAtDirectory (Directory, DirName, Entries, NumEntries);

  DEBUG ((
    (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) ? DEBUG_WARN : DEBUG_INFO,
    "LNX: ProbeGrub %s - %r\n",
    DirName,
    Status
    ));
    
  return Status;
}

EFI_STATUS
ProbeGrub (
  IN   EFI_FILE_PROTOCOL        *RootDirectory,
  OUT  OC_PICKER_ENTRY          **Entries,
  OUT  UINTN                    *NumEntries
  )
{
  EFI_STATUS                    Status;
  EFI_FILE_PROTOCOL             *AdditionalScanDirectory;

  Status = DoProbeGrub (RootDirectory, ROOT_DIR, Entries, NumEntries);
  if (EFI_ERROR (Status)) {
    Status = OcSafeFileOpen (RootDirectory, &AdditionalScanDirectory, BOOT_DIR, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR (Status)) {
      Status = DoProbeGrub (AdditionalScanDirectory, BOOT_DIR, Entries, NumEntries);
      AdditionalScanDirectory->Close (AdditionalScanDirectory);
    }
  }
  
  return Status;
}
