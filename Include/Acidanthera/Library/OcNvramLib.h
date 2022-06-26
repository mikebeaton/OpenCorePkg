/** @file
  Copyright (C) 2016-2022, vit9696, mikebeaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#ifndef OC_NVRAM_LIB_H
#define OC_NVRAM_LIB_H

#include <Uefi.h>
#include <Library/OcConfigurationLib.h>
#include <Library/OcStorageLib.h>

#define OPEN_CORE_NVRAM_ROOT_PATH  L"NVRAM"

#define OPEN_CORE_NVRAM_FILENAME       L"nvram.plist"

#define OPEN_CORE_NVRAM_FALLBACK_FILENAME  L"nvram.fallback"

#define OPEN_CORE_NVRAM_USED_FILENAME      L"nvram.used"

#define OPEN_CORE_NVRAM_ATTR  (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

#define OPEN_CORE_NVRAM_NV_ATTR  (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE)

#define OPEN_CORE_INT_NVRAM_ATTR  EFI_VARIABLE_BOOTSERVICE_ACCESS

/**
  Load NVRAM compatibility support.

  @param[in]  Storage   OpenCore storage.
  @param[in]  Config    OpenCore configuration.
**/
VOID
OcLoadNvramSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config
  );

/**
  Save to emulated NVRAM using installed protocol when present.
**/
VOID
EFIAPI
OcSaveLegacyNvram (
  VOID
  );

/**
  Reset emulated NVRAM using installed protocol when present.
  If protocol is present, does not return and restarts system.
**/
VOID
EFIAPI
OcResetLegacyNvram (
  VOID
  );

/**
  Switch to fallback emulated NVRAM using installed protocol when present.
**/
VOID
EFIAPI
OcSwitchToFallbackLegacyNvram (
  VOID
  );

/**
  Test NVRAM GUID against legacy schema.

  @param[in]  AsciiVariableGuid   Guid to test in ASCII format.
  @param[out] VariableGuid        On success AsciiVariableGuid converted to GUID format.
  @param[in]  Schema              Schema to test against.
  @param[out] SchemaEntry         On success list of allowed variable names for this GUID.

  @result EFI_SUCCESS If at least some variables are allowed under this GUID.
**/
EFI_STATUS
OcProcessVariableGuid (
  IN  CONST CHAR8            *AsciiVariableGuid,
  OUT GUID                   *VariableGuid,
  IN  OC_NVRAM_LEGACY_MAP    *Schema  OPTIONAL,
  OUT OC_NVRAM_LEGACY_ENTRY  **SchemaEntry  OPTIONAL
  );

/**
  Test NVRAM variable name against legacy schema.

  @param[in]  SchemaEntry         List of allowed names.
  @param[in]  VariableGuid        Variable GUID (optional, for debug output only).
  @param[in]  VariableName        Variable name.
  @param[in]  StringFormat        Is VariableName Ascii or Unicode?
**/
BOOLEAN
OcVariableIsAllowedBySchemaEntry (
  IN OC_NVRAM_LEGACY_ENTRY  *SchemaEntry,
  IN EFI_GUID               *VariableGuid OPTIONAL,
  IN CONST VOID             *VariableName,
  IN OC_STRING_FORMAT       StringFormat
  );

/**
  Set NVRAM variable - for internal use at NVRAM setup only.

  @param[in]  AsciiVariableName   Variable name.
  @param[in]  VariableGuid        Variably Guid.
  @param[in]  Attributes          Attributes.
  @param[in]  VariableSize        Data size.
  @param[in]  VariableData        Data.
  @param[in]  SchemaEntry         Optional schema to filter by.
  @param[in]  Overwrite           If TRUE pre-existing variables can be overwritten.
**/
VOID
OcDirectSetNvramVariable (
  IN CONST CHAR8            *AsciiVariableName,
  IN EFI_GUID               *VariableGuid,
  IN UINT32                 Attributes,
  IN UINT32                 VariableSize,
  IN VOID                   *VariableData,
  IN OC_NVRAM_LEGACY_ENTRY  *SchemaEntry OPTIONAL,
  IN BOOLEAN                Overwrite
  );

/**
  Get EFI boot option from specified namespace.

  @param[out] OptionSize      Boot option size.
  @param[in] BootOption       Which boot option to return.
  @param[in] BootGuid         Boot namespace to use (OC or default).

  @retval EFI boot option data.
**/
EFI_LOAD_OPTION *
OcGetBootOptionData (
  OUT UINTN           *OptionSize,
  IN  UINT16          BootOption,
  IN  CONST EFI_GUID  *BootGuid
  );

/**
  Resets selected NVRAM variables and reboots the system.

  @param[in]     PreserveBoot       Should reset preserve Boot### entries.

  @retval EFI_SUCCESS, or error returned by called code.
**/
EFI_STATUS
OcResetNvram (
  IN     BOOLEAN  PreserveBoot
  );

/**
  Perform NVRAM UEFI variable deletion.
**/
VOID
OcDeleteVariables (
  IN BOOLEAN  PreserveBoot
  );

/**
  Process variable result.
**/
typedef enum _OC_PROCESS_VARIABLE_RESULT {
  OcProcessVariableContinue,
  OcProcessVariableRestart,
  OcProcessVariableAbort
} OC_PROCESS_VARIABLE_RESULT;

/**
  Process variable during OcScanVariables.
  Any filtering of which variables to use is done within this function.

  @param[in]     Guid               Variable GUID.
  @param[in]     Name               Variable Name.
  @param[in]     Context            Caller-provided context.

  @retval Indicates whether the scan should continue, restart or abort.
**/
typedef
OC_PROCESS_VARIABLE_RESULT
(EFIAPI *OC_PROCESS_VARIABLE) (
  IN EFI_GUID           *Guid,
  IN CHAR16             *Name,
  IN VOID               *Context OPTIONAL
  );

/**
  Apply function to each NVRAM variable.

  @param[in]     ProcessVariable    Function to apply.
  @param[in]     Context            Caller-provided context.
**/
VOID
OcScanVariables (
  IN OC_PROCESS_VARIABLE  ProcessVariable,
  IN VOID                 *Context
  );

/**
  Get current SIP setting.

  @param[out]     CsrActiveConfig    Returned csr-active-config variable; uninitialised if variable
                                     not found, or other error.
  @param[out]     Attributes         If not NULL, a pointer to the memory location to return the
                                     attributes bitmask for the variable; uninitialised if variable
                                     not found, or other error.

  @retval EFI_SUCCESS, EFI_NOT_FOUND, or other error returned by called code.
**/
EFI_STATUS
OcGetSip (
  OUT UINT32  *CsrActiveConfig,
  OUT UINT32  *Attributes          OPTIONAL
  );

/**
  Set current SIP setting.

  @param[in]      CsrActiveConfig    csr-active-config value to set, or NULL to clear the variable.
  @param[in]      Attributes         Attributes to apply.

  @retval EFI_SUCCESS, EFI_NOT_FOUND, or other error returned by called code.
**/
EFI_STATUS
OcSetSip (
  IN  UINT32  *CsrActiveConfig,
  IN  UINT32  Attributes
  );

/**
  Is SIP enabled?

  @param[in]      GetStatus          Return status from previous OcGetSip or gRT->GetVariable call.
  @param[in]      CsrActiveConfig    csr-active-config value from previous OcGetSip or gRT->GetVariable call.
                                     This value is never used unless GetStatus is EFI_SUCCESS.

  @retval TRUE if SIP should be considered enabled based on the passed values.
**/
BOOLEAN
OcIsSipEnabled (
  IN  EFI_STATUS  GetStatus,
  IN  UINT32      CsrActiveConfig
  );

/**
  Toggle SIP.

  @param[in]      CsrActiveConfig    The csr-active-config value to use to disable SIP, if it was previously enabled.

  @retval TRUE on successful operation.
**/
EFI_STATUS
OcToggleSip (
  IN  UINT32  CsrActiveConfig
  );

#endif // OC_NVRAM_LIB_H
