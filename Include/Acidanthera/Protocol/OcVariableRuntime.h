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

OC_MAP_STRUCTORS (OC_NVRAM_STORAGE_MAP)
OC_STRUCTORS (OC_NVRAM_STORAGE, ())

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
  Load NVRAM from storage.

  @param[in]  FileSystem        OpenCore root filesystem.
  @param[in]  Schema            Schema specifying the NVRAM which values may be loaded if present.
  @param[in]  WriteFlash        TRUE if vars should be written as non-volatile.
  @param[in]  LegacyOverwrite   TRUE if existing vars should be overwritten.

  @retval EFI_NOT_FOUND         Invalid or missing NVRAM storage.
  @retval EFI_UNSUPPORTED       Invalid NVRAM storage contents.
  @retval EFI_OUT_OF_RESOURCES  Out of memory.
  @retval EFI_SUCCESS           NVRAM contents were successfully loaded from storage.
**/
typedef
EFI_STATUS
EFIAPI
(EFIAPI *OC_VARIABLE_RUNTIME_PROTOCOL_LOAD_NVRAM)(
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem,
  IN OC_NVRAM_LEGACY_MAP              *Schema,
  IN BOOLEAN                          WriteFlash,
  IN BOOLEAN                          LegacyOverwrite
  );

/**
  The structure exposed by OC_VARIABLE_RUNTIME_PROTOCOL.
**/
struct OC_VARIABLE_RUNTIME_PROTOCOL_ {
  //
  // Protocol revision.
  //
  UINTN                                           Revision;
  //
  // Load NVRAM.
  //
  OC_VARIABLE_RUNTIME_PROTOCOL_LOAD_NVRAM         LoadNvram;
  //
  // Save NVRAM.
  //
  ////OC_VARIABLE_RUNTIME_PROTOCOL_SAVE_NVRAM   SaveNvram;
  //
  // Reset NVRAM.
  //
  ////OC_VARIABLE_RUNTIME_PROTOCOL_RESET_NVRAM  ResetNvram;
};

extern EFI_GUID  gOcVariableRuntimeProtocolGuid;

#endif // OC_VARIABLE_RUNTIME_PROTOCOL_H
