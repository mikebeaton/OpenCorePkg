/** @file
  Copyright (C) 2022, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#ifndef OC_VARIABLE_RUNTIME_PROTOCOL_H
#define OC_VARIABLE_RUNTIME_PROTOCOL_H

#include <Uefi.h>
#include <Library/OcConfigurationLib.h>
#include <Library/OcTemplateLib.h>

/**
  Structure declaration for loaded nvram contents.
**/
#define OC_NVRAM_STORAGE_MAP_FIELDS(_, __) \
  OC_MAP (OC_STRING, OC_ASSOC, _, __)
OC_DECLARE (OC_NVRAM_STORAGE_MAP)

#define OC_NVRAM_STORAGE_FIELDS(_, __) \
  _(UINT32                      , Version  ,     , 0                                       , () ) \
  _(OC_NVRAM_STORAGE_MAP        , Add      ,     , OC_CONSTR (OC_NVRAM_STORAGE_MAP, _, __) , OC_DESTR (OC_NVRAM_STORAGE_MAP))
OC_DECLARE (OC_NVRAM_STORAGE)

/**
  Variable runtime protocol version.
**/
#define OC_VARIABLE_RUNTIME_PROTOCOL_REVISION  1

//
// OC_VARIABLE_RUNTIME_PROTOCOL_GUID
// 3DBA852A-2645-4184-9571-E60C2BFD724C
//
#define OC_VARIABLE_RUNTIME_PROTOCOL_GUID  \
  { 0x3DBA852A, 0x2645, 0x4184, \
    { 0x95, 0x71, 0xE6, 0x0C, 0x2B, 0xFD, 0x72, 0x4C } }

/**
  Forward declaration of OC_VARIABLE_RUNTIME_PROTOCOL structure.
**/
typedef struct OC_VARIABLE_RUNTIME_PROTOCOL_ OC_VARIABLE_RUNTIME_PROTOCOL;

/**
  Load NVRAM from storage, applying legacy filter from config.

  @param[in]  StorageContext      OpenCore storage context.
  @param[in]  NvramConfig         OpenCore NVRAM config.

  @retval EFI_INVALID_PARAMETER   StorageContext or NvramConfig is NULL.
  @retval EFI_ALREADY_STARTED     Has been called already.
  @retval EFI_NOT_FOUND           Invalid or missing NVRAM storage.
  @retval EFI_UNSUPPORTED         Invalid NVRAM storage contents.
  @retval EFI_SUCCESS             On success.
**/
typedef
EFI_STATUS
EFIAPI
(EFIAPI *OC_VARIABLE_RUNTIME_PROTOCOL_LOAD_NVRAM)(
  IN OC_STORAGE_CONTEXT               *StorageContext,
  IN OC_NVRAM_CONFIG                  *NvramConfig
  );

/**
  Save NVRAM to storage, applying legacy filter from config.

  @param[in]  Storage             OpenCore storage context.
  @param[in]  NvramConfig         OpenCore NVRAM config.

  @retval EFI_NOT_READY           If called before LoadNvram.
  @retval EFI_NOT_FOUND           Invalid or missing NVRAM storage.
  @retval EFI_UNSUPPORTED         Invalid NVRAM storage contents.
  @retval EFI_OUT_OF_RESOURCES    Out of memory.
  @retval other                   Other error from child function.
  @retval EFI_SUCCESS             On success.
**/
typedef
EFI_STATUS
EFIAPI
(EFIAPI *OC_VARIABLE_RUNTIME_PROTOCOL_SAVE_NVRAM)(
  VOID
  );

/**
  Reset NVRAM.

  @param[in]  Storage             OpenCore storage context.

  @retval EFI_NOT_READY           If called before LoadNvram.
  @retval EFI_NOT_FOUND           Invalid or missing NVRAM storage.
  @retval other                   Other error from child function.
  @retval EFI_SUCCESS             On success.
**/
typedef
EFI_STATUS
EFIAPI
(EFIAPI *OC_VARIABLE_RUNTIME_PROTOCOL_RESET_NVRAM)(
  VOID
  );

/**
  Switch to fallback NVRAM. Workaround for the fact that we cannot save NVRAM
  changes during macOS installer reboots (which never start launch daemon).
  Reverts to previous NVRAM file, assuming this is present.
  In existing implementation this works in combination with Launchd.command,
  which renames previous nvram.plist as nvram.fallback on each save of new file.

  @param[in]  Storage             OpenCore storage context.

  @retval EFI_NOT_READY           If called before LoadNvram.
  @retval EFI_NOT_FOUND           Invalid or missing NVRAM storage.
  @retval EFI_ALREADY_STARTED     Already at fallback NVRAM.
  @retval EFI_OUT_OF_RESOURCES    Out of memory.
  @retval other                   Other error from child function.
  @retval EFI_SUCCESS             On success.
**/
typedef
EFI_STATUS
EFIAPI
(EFIAPI *OC_VARIABLE_RUNTIME_PROTOCOL_SWITCH_TO_FALLBACK)(
  VOID
  );

/**
  The structure exposed by OC_VARIABLE_RUNTIME_PROTOCOL.
**/
struct OC_VARIABLE_RUNTIME_PROTOCOL_ {
  //
  // Protocol revision.
  //
  UINTN                                             Revision;
  //
  // Load NVRAM.
  //
  OC_VARIABLE_RUNTIME_PROTOCOL_LOAD_NVRAM           LoadNvram;
  //
  // Save NVRAM.
  //
  OC_VARIABLE_RUNTIME_PROTOCOL_SAVE_NVRAM           SaveNvram;
  //
  // Reset NVRAM.
  //
  OC_VARIABLE_RUNTIME_PROTOCOL_RESET_NVRAM          ResetNvram;
  //
  // Reset NVRAM.
  //
  OC_VARIABLE_RUNTIME_PROTOCOL_SWITCH_TO_FALLBACK   SwitchToFallback;
};

extern EFI_GUID  gOcVariableRuntimeProtocolGuid;

#endif // OC_VARIABLE_RUNTIME_PROTOCOL_H
