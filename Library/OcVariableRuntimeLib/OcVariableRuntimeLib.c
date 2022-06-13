/** @file
  Save, load and delete emulated NVRAM from file storage.

  Copyright (c) 2019-2022, vit9696, mikebeaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcNvramLib.h>
#include <Library/OcSerializeLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/OcVariableRuntime.h>

#define BASE64_CHUNK_SIZE (52)

typedef struct {
  UINT8                         *DataBuffer;
  UINTN                         DataBufferSize;
  CHAR8                         *Base64Buffer;
  UINTN                         Base64BufferSize;
  OC_ASCII_STRING_BUFFER        *StringBuffer;
  GUID                          SectionGuid;
  OC_NVRAM_LEGACY_ENTRY         *SchemaEntry;
  EFI_STATUS                    Status;
} NVRAM_SAVE_CONTEXT;

/**
  Version check for NVRAM file. Not the same as protocol revision.
**/
#define OC_NVRAM_STORAGE_VERSION  1

/**
  Structors for loaded NVRAM contents must be declared only once.
**/
OC_MAP_STRUCTORS (OC_NVRAM_STORAGE_MAP)
OC_STRUCTORS (OC_NVRAM_STORAGE, ())

/**
  Schema definition for NVRAM file.
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
OC_STORAGE_CONTEXT
*mStorageContext = NULL;

STATIC
OC_NVRAM_CONFIG
*mNvramConfig = NULL;

STATIC
EFI_STATUS
LocateNvramDir (
  OUT EFI_FILE_PROTOCOL **NvramDir
  )
{
  EFI_STATUS                    Status;
  EFI_FILE_PROTOCOL             *Root;

  if (mStorageContext == NULL || mNvramConfig == NULL) {
    return EFI_NOT_READY;
  }

  if (mStorageContext->FileSystem == NULL) {
    DEBUG ((DEBUG_WARN, "VAR: No file system\n"));
    return EFI_NOT_FOUND;
  }

  Status = mStorageContext->FileSystem->OpenVolume (mStorageContext->FileSystem, &Root);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: Invalid root volume - %r\n", Status));
    return EFI_NOT_FOUND;
  }

  Status = OcSafeFileOpen (
    Root,
    NvramDir,
    OPEN_CORE_NVRAM_ROOT_PATH,
    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
    EFI_FILE_DIRECTORY
    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: Cannot open %s - %r\n", OPEN_CORE_NVRAM_ROOT_PATH, Status));
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
LoadNvram (
  IN OC_STORAGE_CONTEXT               *StorageContext,
  IN OC_NVRAM_CONFIG                  *NvramConfig
  )
{
  EFI_STATUS                    Status;
  EFI_FILE_PROTOCOL             *NvramDir;
  UINT8                         *FileBuffer;
  UINT32                        FileSize;
  BOOLEAN                       IsValid;
  OC_NVRAM_STORAGE              NvramStorage;
  UINT32                        GuidIndex;
  UINT32                        VariableIndex;
  GUID                          VariableGuid;
  OC_ASSOC                      *VariableMap;
  OC_NVRAM_LEGACY_ENTRY         *SchemaEntry;

  DEBUG ((DEBUG_INFO, "VAR: Loading NVRAM...\n"));

  if (mStorageContext != NULL || mNvramConfig != NULL) {
    return EFI_ALREADY_STARTED;
  }

  if (StorageContext == NULL || NvramConfig == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  mStorageContext = StorageContext;
  mNvramConfig = NvramConfig;

  Status = LocateNvramDir (&NvramDir);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FileBuffer = OcReadFileFromDirectory (NvramDir, OPEN_CORE_NVRAM_FILENAME, &FileSize, BASE_1MB);
  if (FileBuffer == NULL) {
    DEBUG ((DEBUG_INFO, "VAR: Trying fallback NVRAM data\n"));
    FileBuffer = OcReadFileFromDirectory (NvramDir, OPEN_CORE_NVRAM_FALLBACK_FILENAME, &FileSize, BASE_1MB);
  }
  NvramDir->Close (NvramDir);
  if (FileBuffer == NULL) {
    DEBUG ((DEBUG_WARN, "VAR: Nvram data not found or not readable\n"));
    return EFI_NOT_FOUND;
  }

  OC_NVRAM_STORAGE_CONSTRUCT (&NvramStorage, sizeof (NvramStorage));
  IsValid = ParseSerialized (&NvramStorage, &mNvramStorageRootSchema, FileBuffer, FileSize, NULL);
  FreePool (FileBuffer);

  if (!IsValid) {
    DEBUG ((DEBUG_WARN, "VAR: Invalid NVRAM data\n"));
    OC_NVRAM_STORAGE_DESTRUCT (&NvramStorage, sizeof (NvramStorage));
    return EFI_UNSUPPORTED;
  }

  if (NvramStorage.Version != OC_NVRAM_STORAGE_VERSION) {
    DEBUG ((
      DEBUG_WARN,
      "VAR: Incompatible NVRAM data, version %u vs %u\n",
      NvramStorage.Version,
      OC_NVRAM_STORAGE_VERSION
      ));
    OC_NVRAM_STORAGE_DESTRUCT (&NvramStorage, sizeof (NvramStorage));
    return EFI_UNSUPPORTED;
  }

  for (GuidIndex = 0; GuidIndex < NvramStorage.Add.Count; ++GuidIndex) {
    Status = OcProcessVariableGuid (
               OC_BLOB_GET (NvramStorage.Add.Keys[GuidIndex]),
               &VariableGuid,
               &mNvramConfig->Legacy,
               &SchemaEntry
               );

    if (EFI_ERROR (Status)) {
      continue;
    }

    VariableMap = NvramStorage.Add.Values[GuidIndex];

    for (VariableIndex = 0; VariableIndex < VariableMap->Count; ++VariableIndex) {
      OcDirectSetNvramVariable (
        OC_BLOB_GET (VariableMap->Keys[VariableIndex]),
        &VariableGuid,
        mNvramConfig->WriteFlash ? OPEN_CORE_NVRAM_NV_ATTR : OPEN_CORE_NVRAM_ATTR,
        VariableMap->Values[VariableIndex]->Size,
        OC_BLOB_GET (VariableMap->Values[VariableIndex]),
        SchemaEntry,
        mNvramConfig->LegacyOverwrite
        );
    }
  }

  OC_NVRAM_STORAGE_DESTRUCT (&NvramStorage, sizeof (NvramStorage));

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
DeleteFile (
  IN EFI_FILE_PROTOCOL             *NvramDir,
  IN CONST CHAR16                  *FileName
  )
{
  EFI_STATUS                    Status;
  EFI_FILE_PROTOCOL             *File;

  Status = OcSafeFileOpen (NvramDir, &File, OPEN_CORE_NVRAM_FILENAME, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status)) {
    Status = File->Delete (File);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "VAR: Cannot delete %s! - %r\n", FileName, Status));
    }
  }
  return Status;
}

//
// Serialize one section at a time, NVRAM scan per section.
//
STATIC
OC_PROCESS_VARIABLE_RESULT
EFIAPI
SerializeSectionVariables (
  IN EFI_GUID        *Guid,
  IN CHAR16          *Name,
  IN VOID            *Context
  )
{
  EFI_STATUS              Status;
  NVRAM_SAVE_CONTEXT      *SaveContext;
  UINT32                  Attributes;
  UINTN                   DataSize;
  UINTN                   Base64Size;
  UINTN                   Base64Pos;

  ASSERT (Context != NULL);
  SaveContext = Context;

  if (!CompareGuid (Guid, &SaveContext->SectionGuid)) {
    return OcProcessVariableContinue;
  }

  if (!OcVariableIsAllowedBySchemaEntry (SaveContext->SchemaEntry, Name, OcStringFormatUnicode)) {
    DEBUG ((DEBUG_INFO, "VAR: Saving NVRAM %g:%s is not permitted\n", Guid, Name));
    return OcProcessVariableContinue;
  }

  do {
    DataSize = SaveContext->DataBufferSize;
    Status = gRT->GetVariable (
                    Name,
                    Guid,
                    &Attributes,
                    &DataSize,
                    SaveContext->DataBuffer
                    );
    if (Status == EFI_BUFFER_TOO_SMALL) {
      while (DataSize > SaveContext->DataBufferSize) {
        if (OcOverflowMulUN (SaveContext->DataBufferSize, 2, &SaveContext->DataBufferSize)) {
          SaveContext->Status = EFI_OUT_OF_RESOURCES;
          return OcProcessVariableAbort;
        }
      }
      FreePool (SaveContext->DataBuffer);
      SaveContext->DataBuffer = AllocatePool (SaveContext->DataBufferSize);
      if (SaveContext->DataBuffer == NULL) {
        SaveContext->Status = EFI_OUT_OF_RESOURCES;
        return OcProcessVariableAbort;
      }
    }
  } while (Status == EFI_BUFFER_TOO_SMALL);

  if (EFI_ERROR (Status)) {
    SaveContext->Status = Status;
    return OcProcessVariableAbort;
  }

  //
  // Only save non-volatile variables; also, match launchd script and only save
  // variables which it can save, i.e. runtime accessible.
  //
  if (((Attributes & EFI_VARIABLE_RUNTIME_ACCESS) == 0)
    || ((Attributes & EFI_VARIABLE_NON_VOLATILE) == 0)) {
    DEBUG ((DEBUG_INFO, "VAR: Saving NVRAM %g:%s skipped due to attributes 0x%X\n", Guid, Name, Attributes));
    return OcProcessVariableContinue;
  }

  Base64Size = 0;
  Base64Encode (SaveContext->DataBuffer, DataSize, NULL, &Base64Size);
  if (Base64Size > SaveContext->Base64BufferSize) {
    while (Base64Size > SaveContext->Base64BufferSize) {
      if (OcOverflowMulUN (SaveContext->Base64BufferSize, 2, &SaveContext->Base64BufferSize)) {
        SaveContext->Status = EFI_OUT_OF_RESOURCES;
        return OcProcessVariableAbort;
      }
    }
    FreePool (SaveContext->Base64Buffer);
    SaveContext->Base64Buffer = AllocatePool (SaveContext->Base64BufferSize);
    if (SaveContext->Base64Buffer == NULL) {
      SaveContext->Status = EFI_OUT_OF_RESOURCES;
      return OcProcessVariableAbort;
    }
  }
  Base64Encode (SaveContext->DataBuffer, DataSize, SaveContext->Base64Buffer, &Base64Size);

  //
  // %c works around BasePrintLibSPrintMarker converting \n to \r\n.
  //
  Status = OcAsciiStringBufferSPrint (
    SaveContext->StringBuffer,
    "\t\t\t<key>%s</key>%c"
    "\t\t\t<data>%c",
    Name, '\n', '\n'
    );
  if (EFI_ERROR (Status)) {
    SaveContext->Status = Status;
    return OcProcessVariableAbort;
  }

  for (Base64Pos = 0; Base64Pos < (Base64Size - 1); Base64Pos += BASE64_CHUNK_SIZE) {
    Status = OcAsciiStringBufferAppend (
      SaveContext->StringBuffer,
      "\t\t\t"
      );
    if (EFI_ERROR (Status)) {
      SaveContext->Status = Status;
      return OcProcessVariableAbort;
    }
    Status = OcAsciiStringBufferAppendN (
      SaveContext->StringBuffer,
      &SaveContext->Base64Buffer[Base64Pos],
      BASE64_CHUNK_SIZE
      );
    if (EFI_ERROR (Status)) {
      SaveContext->Status = Status;
      return OcProcessVariableAbort;
    }
    Status = OcAsciiStringBufferAppend (
      SaveContext->StringBuffer,
      "\n"
      );
    if (EFI_ERROR (Status)) {
      SaveContext->Status = Status;
      return OcProcessVariableAbort;
    }
  }

  Status = OcAsciiStringBufferAppend (
    SaveContext->StringBuffer,
    "\t\t\t</data>\n"
    );
  if (EFI_ERROR (Status)) {
    SaveContext->Status = Status;
    return OcProcessVariableAbort;
  }
   
  return OcProcessVariableContinue;
}

STATIC
EFI_STATUS
EFIAPI
SaveNvram (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_FILE_PROTOCOL             *NvramDir;
  UINT32                        GuidIndex;
  NVRAM_SAVE_CONTEXT            Context;

  DEBUG ((DEBUG_INFO, "VAR: Saving NVRAM...\n"));

  Status = LocateNvramDir (&NvramDir);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Context.Status = EFI_SUCCESS;

  Context.DataBufferSize = BASE_1KB;
  Context.DataBuffer = AllocatePool (Context.DataBufferSize);
  if (Context.DataBuffer == NULL) {
    NvramDir->Close (NvramDir);
    return EFI_OUT_OF_RESOURCES;
  }

  Context.Base64BufferSize = BASE_1KB;
  Context.Base64Buffer = AllocatePool (Context.Base64BufferSize);
  if (Context.Base64Buffer == NULL) {
    NvramDir->Close (NvramDir);
    FreePool (Context.DataBuffer);
    return EFI_OUT_OF_RESOURCES;
  }

  Context.StringBuffer = OcAsciiStringBufferInit ();
  if (Context.StringBuffer == NULL) {
    NvramDir->Close (NvramDir);
    FreePool (Context.DataBuffer);
    FreePool (Context.Base64Buffer);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = OcAsciiStringBufferAppend (
    Context.StringBuffer,
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>Add</key>\n"
    "\t<dict>\n"
    );

  if (EFI_ERROR (Status)) {
    NvramDir->Close (NvramDir);
    FreePool (Context.DataBuffer);
    FreePool (Context.Base64Buffer);
    OcAsciiStringBufferFree (&Context.StringBuffer);
    return Status;
  }

  for (GuidIndex = 0; GuidIndex < mNvramConfig->Legacy.Count; ++GuidIndex) {
    Status = OcProcessVariableGuid (
      OC_BLOB_GET (mNvramConfig->Legacy.Keys[GuidIndex]),
      &Context.SectionGuid,
      &mNvramConfig->Legacy,
      &Context.SchemaEntry
      );
    if (EFI_ERROR (Status)) {
      Status = EFI_SUCCESS;
      continue;
    }

    Status = OcAsciiStringBufferSPrint (
      Context.StringBuffer,
      "\t\t<key>%g</key>%c"
      "\t\t<dict>%c",
      Context.SectionGuid, '\n', '\n'
      );
    if (EFI_ERROR (Status)) {
      break;
    }

    OcScanVariables (SerializeSectionVariables, &Context);
    Status = Context.Status;
    if (EFI_ERROR (Status)) {
      break;
    }

    Status = OcAsciiStringBufferAppend (
      Context.StringBuffer,
      "\t\t</dict>\n"
      );
    if (EFI_ERROR (Status)) {
      break;
    }
  }

  FreePool (Context.DataBuffer);
  FreePool (Context.Base64Buffer);

  if (!EFI_ERROR (Status)) {
    Status = OcAsciiStringBufferSPrint (
      Context.StringBuffer,
      "\t</dict>%c"
      "\t<key>Version</key>%c"
      "\t<integer>%u</integer>%c"
      "</dict>%c"
      "</plist>%c",
      '\n', '\n', OC_NVRAM_STORAGE_VERSION,
      '\n', '\n', '\n'
      );
  }
  if (EFI_ERROR (Status)) {
    NvramDir->Close (NvramDir);
    OcAsciiStringBufferFree (&Context.StringBuffer);
    return Status;
  }

  Status = DeleteFile (NvramDir, OPEN_CORE_NVRAM_FILENAME);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: Error deleting %s - %r\n", OPEN_CORE_NVRAM_FILENAME, Status));
  }

  Status = OcSetFileData (
    NvramDir,
    OPEN_CORE_NVRAM_FILENAME,
    Context.StringBuffer->String,
    Context.StringBuffer->StringLength
    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: Error writing %s - %r\n", OPEN_CORE_NVRAM_FILENAME, Status));
  }

  OcAsciiStringBufferFree (&Context.StringBuffer);
  NvramDir->Close (NvramDir);

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
ResetNvram (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_STATUS                    AltStatus;
  EFI_FILE_PROTOCOL             *NvramDir;

  DEBUG ((DEBUG_INFO, "VAR: Resetting NVRAM...\n"));

  Status = LocateNvramDir (&NvramDir);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = DeleteFile (NvramDir, OPEN_CORE_NVRAM_FILENAME);
  if (Status == EFI_NOT_FOUND) {
    Status = EFI_SUCCESS;
  }

  AltStatus = DeleteFile (NvramDir, OPEN_CORE_NVRAM_FALLBACK_FILENAME);
  if (AltStatus == EFI_NOT_FOUND) {
    AltStatus = EFI_SUCCESS;
  }

  NvramDir->Close (NvramDir);

  return EFI_ERROR (Status) ? Status : AltStatus;
}

STATIC
EFI_STATUS
EFIAPI
SwitchToFallback (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_FILE_PROTOCOL             *NvramDir;
  UINT8                         *FileBuffer;
  UINT32                        FileSize;
  EFI_FILE_PROTOCOL             *FallbackFile;

  DEBUG ((DEBUG_INFO, "VAR: Switching to fallback NVRAM...\n"));

  Status = LocateNvramDir (&NvramDir);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = OcSafeFileOpen (NvramDir, &FallbackFile, OPEN_CORE_NVRAM_FALLBACK_FILENAME, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: %s cannot be opened, not switching to fallback! - %r\n", OPEN_CORE_NVRAM_FALLBACK_FILENAME, Status));
    NvramDir->Close (NvramDir);
    return Status;
  }
  FallbackFile->Close (FallbackFile);

  FileBuffer = OcReadFileFromDirectory (NvramDir, OPEN_CORE_NVRAM_FILENAME, &FileSize, BASE_1MB);
  if (FileBuffer == NULL) {
    DEBUG ((DEBUG_INFO, "VAR: %s cannot be opened, already switched to fallback?\n", OPEN_CORE_NVRAM_FILENAME));
    NvramDir->Close (NvramDir);
    return EFI_NOT_FOUND;
  }

  Status = DeleteFile (NvramDir, OPEN_CORE_NVRAM_USED_FILENAME);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: Failure deleting %s - %r\n", OPEN_CORE_NVRAM_USED_FILENAME, Status));
  }

  Status = DeleteFile (NvramDir, OPEN_CORE_NVRAM_FILENAME);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: Failure deleting %s - %r\n", OPEN_CORE_NVRAM_FILENAME, Status));
  }

  Status = OcSetFileData (
    NvramDir,
    OPEN_CORE_NVRAM_USED_FILENAME,
    FileBuffer,
    FileSize
    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: Error writing %s - %r\n", OPEN_CORE_NVRAM_USED_FILENAME, Status));
  }

  NvramDir->Close (NvramDir);
  FreePool (FileBuffer);

  return Status;
}

STATIC
OC_VARIABLE_RUNTIME_PROTOCOL
mOcVariableRuntimeProtocol = {
                                                      OC_VARIABLE_RUNTIME_PROTOCOL_REVISION,
  (OC_VARIABLE_RUNTIME_PROTOCOL_LOAD_NVRAM)           LoadNvram,
  (OC_VARIABLE_RUNTIME_PROTOCOL_SAVE_NVRAM)           SaveNvram,
  (OC_VARIABLE_RUNTIME_PROTOCOL_RESET_NVRAM)          ResetNvram,
  (OC_VARIABLE_RUNTIME_PROTOCOL_SWITCH_TO_FALLBACK)   SwitchToFallback
};

EFI_STATUS
EFIAPI
OcVariableRuntimeLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gOcVariableRuntimeProtocolGuid,
                  &mOcVariableRuntimeProtocol,
                  NULL
                  );
}
