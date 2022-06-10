/** @file
  Copyright (C) 2022, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#ifndef VARIABLE_RUNTIME_INTERNAL_H
#define VARIABLE_RUNTIME_INTERNAL_H

#include <Uefi.h>
#include <Library/OcConfigurationLib.h>

EFI_STATUS
InternalProcessVariableGuid (
  IN  CONST CHAR8            *AsciiVariableGuid,
  OUT GUID                   *VariableGuid,
  IN  OC_NVRAM_LEGACY_MAP    *Schema  OPTIONAL,
  OUT OC_NVRAM_LEGACY_ENTRY  **SchemaEntry  OPTIONAL
  );

VOID
InternalSetNvramVariable (
  IN CONST CHAR8            *AsciiVariableName,
  IN EFI_GUID               *VariableGuid,
  IN UINT32                 Attributes,
  IN UINT32                 VariableSize,
  IN VOID                   *VariableData,
  IN OC_NVRAM_LEGACY_ENTRY  *SchemaEntry,
  IN BOOLEAN                Overwrite
  );

#endif // VARIABLE_RUNTIME_INTERNAL_H
