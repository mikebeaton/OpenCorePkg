/** @file
  Save, load and delete emulated NVRAM from file storage.

  Copyright (c) 2019-2022, vit9696, mikebeaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "VariableRuntimeInternal.h"

#include <Library/OcMainLib.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcSerializeLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/OcVariableRuntime.h>

/**
  Version check for nvram file. Not necessarily the same as protocol revision.
**/
#define OC_NVRAM_STORAGE_VERSION  1

/**
  Structors for loaded nvram contents must be declared only once.
**/
OC_MAP_STRUCTORS (OC_NVRAM_STORAGE_MAP)
OC_STRUCTORS (OC_NVRAM_STORAGE, ())

/**
  Schema definition for nvram file.
**/

STATIC
OC_SCHEMA
mNvramStorageEntrySchema = OC_SCHEMA_MDATA (NULL);

STATIC
OC_SCHEMA
  mNvramStorageAddSchema = OC_SCHEMA_MAP (NULL, &mNvramStorageEntrySchema);

STATIC
OC_SCHEMA
  mNvramStorageNodesSchema[] = {
  OC_SCHEMA_MAP_IN ("Add",         OC_NVRAM_STORAGE, Add,    &mNvramStorageAddSchema),
  OC_SCHEMA_INTEGER_IN ("Version", OC_NVRAM_STORAGE, Version),
};

STATIC
OC_SCHEMA_INFO
  mNvramStorageRootSchema = {
  .Dict = { mNvramStorageNodesSchema, ARRAY_SIZE (mNvramStorageNodesSchema) }
};

STATIC
EFI_STATUS
EFIAPI
VariableRuntimeProtocolLoadNvram (
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem,
  IN OC_NVRAM_CONFIG                  *NvramConfig
  )
{
  EFI_STATUS                    Status;
  UINT8                         *FileBuffer;
  UINT32                        FileSize;
  BOOLEAN                       IsValid;
  OC_NVRAM_STORAGE              Nvram;
  UINT32                        GuidIndex;
  UINT32                        VariableIndex;
  GUID                          VariableGuid;
  OC_ASSOC                      *VariableMap;
  OC_NVRAM_LEGACY_ENTRY         *SchemaEntry;

  FileBuffer = OcReadFile (FileSystem, OPEN_CORE_NVRAM_PATH, &FileSize, BASE_1MB);
  if (FileBuffer == NULL) {
    DEBUG ((DEBUG_INFO, "OC: Invalid nvram data\n"));
    return EFI_NOT_FOUND;
  }

  OC_NVRAM_STORAGE_CONSTRUCT (&Nvram, sizeof (Nvram));
  IsValid = ParseSerialized (&Nvram, &mNvramStorageRootSchema, FileBuffer, FileSize, NULL);
  FreePool (FileBuffer);

  if (!IsValid || (Nvram.Version != OC_NVRAM_STORAGE_VERSION)) {
    DEBUG ((
      DEBUG_WARN,
      "OC: Incompatible nvram data, version %u vs %d\n",
      Nvram.Version,
      OC_NVRAM_STORAGE_VERSION
      ));
    OC_NVRAM_STORAGE_DESTRUCT (&Nvram, sizeof (Nvram));
    return EFI_UNSUPPORTED;
  }

  for (GuidIndex = 0; GuidIndex < Nvram.Add.Count; ++GuidIndex) {
    Status = InternalProcessVariableGuid (
               OC_BLOB_GET (Nvram.Add.Keys[GuidIndex]),
               &VariableGuid,
               &NvramConfig->Legacy,
               &SchemaEntry
               );

    if (EFI_ERROR (Status)) {
      continue;
    }

    VariableMap = Nvram.Add.Values[GuidIndex];

    for (VariableIndex = 0; VariableIndex < VariableMap->Count; ++VariableIndex) {
      InternalSetNvramVariable (
        OC_BLOB_GET (VariableMap->Keys[VariableIndex]),
        &VariableGuid,
        NvramConfig->WriteFlash ? OPEN_CORE_NVRAM_NV_ATTR : OPEN_CORE_NVRAM_ATTR,
        VariableMap->Values[VariableIndex]->Size,
        OC_BLOB_GET (VariableMap->Values[VariableIndex]),
        SchemaEntry,
        NvramConfig->LegacyOverwrite
        );
    }
  }

  OC_NVRAM_STORAGE_DESTRUCT (&Nvram, sizeof (Nvram));

  return EFI_SUCCESS;
}

STATIC
OC_VARIABLE_RUNTIME_PROTOCOL
mOcVariableRuntimeProtocol = {
                                                    OC_VARIABLE_RUNTIME_PROTOCOL_REVISION,
  (OC_VARIABLE_RUNTIME_PROTOCOL_LOAD_NVRAM)         VariableRuntimeProtocolLoadNvram
};

EFI_STATUS
EFIAPI
OcInstallVariableRuntimeProtocol (
  IN EFI_HANDLE        ImageHandle
  )
{
  return gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gOcVariableRuntimeProtocolGuid,
                  &mOcVariableRuntimeProtocol,
                  NULL
                  );
}
