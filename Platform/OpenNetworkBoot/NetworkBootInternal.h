/** @file
  Copyright (C) 2024, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#ifndef LOAD_FILE_INTERNAL_H
#define LOAD_FILE_INTERNAL_H

#include <Uefi.h>
#include <Uefi/UefiSpec.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/OcFlexArrayLib.h>
#include <Library/OcStringLib.h>
#include <Library/OcVirtualFsLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/HttpBootCallback.h>
#include <Protocol/LoadFile.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/OcBootEntry.h>
#include <Protocol/RamDisk.h>

/**
  Set if we should enforce https only within this driver.
**/
extern BOOLEAN gRequireHttpsUri;

/**
  Return the description for network boot device.

  @param Handle                Controller handle.

  @return  The description string.
**/
CHAR16 *
BmGetNetworkDescription (
  IN EFI_HANDLE  Handle
  );

/**
  Return the boot description for LoadFile

  @param Handle                Controller handle.

  @return  The description string.
**/
CHAR16 *
BmGetLoadFileDescription (
  IN EFI_HANDLE  Handle
  );

/**
  Return the full device path pointing to the load option.

  FilePath may:
  1. Exactly matches to a LoadFile instance.
  2. Cannot match to any LoadFile instance. Wide match is required.
  In either case, the routine may return:
  1. A copy of FilePath when FilePath matches to a LoadFile instance and
     the LoadFile returns a load option buffer.
  2. A new device path with IP and URI information updated when wide match
     happens.
  3. A new device path pointing to a load option in RAM disk.
  In either case, only one full device path is returned for a specified
  FilePath.

  @param FilePath    The media device path pointing to a LoadFile instance.

  @return  The load option buffer.
**/
EFI_DEVICE_PATH_PROTOCOL *
BmExpandLoadFiles (
  IN  EFI_DEVICE_PATH_PROTOCOL  *FilePath,
  OUT VOID                      **Data,
  OUT UINT32                    *DataSize,
  IN  OC_DMG_LOADING_SUPPORT    DmgLoading
  );

/**
  Return the RAM Disk device path created by LoadFile.

  @param FilePath  The source file path.

  @return Callee-to-free RAM Disk device path
**/
EFI_DEVICE_PATH_PROTOCOL *
BmGetRamDiskDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL  *FilePath
  );

/**
  Destroy the RAM Disk.

  The destroy operation includes to call RamDisk.Unregister to
  unregister the RAM DISK from RAM DISK driver, free the memory
  allocated for the RAM Disk.

  @param RamDiskDevicePath    RAM Disk device path.
**/
VOID
BmDestroyRamDisk (
  IN EFI_DEVICE_PATH_PROTOCOL  *RamDiskDevicePath
  );

///
/// CustomRead.c
///

EFI_STATUS
EFIAPI
NetworkBootCustomFree (
  IN  VOID      *Context
  );

EFI_STATUS
EFIAPI
NetworkBootCustomRead (
  IN  OC_STORAGE_CONTEXT                  *Storage,
  IN  OC_BOOT_ENTRY                       *ChosenEntry,
  OUT VOID                                **Data,
  OUT UINT32                              *DataSize,
  OUT EFI_DEVICE_PATH_PROTOCOL            **DevicePath,
  OUT EFI_HANDLE                          *StorageHandle,
  OUT EFI_DEVICE_PATH_PROTOCOL            **StoragePath,
  IN  OC_DMG_LOADING_SUPPORT              DmgLoading,
  OUT OC_APPLE_DISK_IMAGE_PRELOAD_CONTEXT *DmgPreloadContext,
  OUT VOID                                **Context
  );

///
/// Uri.c
///

BOOLEAN
HasValidUriProtocol (
  CHAR16 	*Uri
  );

EFI_STATUS
ExtractOtherUri (
  IN  EFI_DEVICE_PATH_PROTOCOL    *DevicePath,
  IN  CHAR8                       *FromExt,
  IN  CHAR8                       *ToExt,
  OUT CHAR8                       **OtherUri,
  IN  BOOLEAN                     OnlySearchForFromExt
  );

BOOLEAN
UriFileHasExtension (
  IN  EFI_DEVICE_PATH_PROTOCOL    *DevicePath,
  IN  CHAR8                       *Ext
  );

EFI_STATUS
HttpBootAddUri (
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  VOID                      *Uri,
  OC_STRING_FORMAT          StringFormat,
  EFI_DEVICE_PATH_PROTOCOL  **UriDevicePath
  );

EFI_EVENT
MonitorHttpBootCallback (
  EFI_HANDLE    LoadFileHandle
  );

#endif // LOAD_FILE_INTERNAL_H
