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

typedef struct {
  VOID                          *ValueBuffer;
  VOID                          *Base64Buffer;
  OC_ASCII_STRING_BUFFER        *StringBuffer;
  GUID                          *EntryGuid;
  OC_NVRAM_LEGACY_ENTRY         *SchemaEntry;
  EFI_STATUS                    Status;
} NVRAM_SAVE_CONTEXT;

/**
  Version check for nvram file. Not the same as protocol revision.
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
EFI_FILE_PROTOCOL *
LocateNvramDir (
  IN OC_STORAGE_CONTEXT               *Storage
  )
{
  EFI_STATUS                    Status;
  EFI_FILE_PROTOCOL             *Root;
  EFI_FILE_PROTOCOL             *NvramDir;

  ASSERT (Storage != NULL);

  if (Storage->FileSystem == NULL) {
    DEBUG ((DEBUG_WARN, "VAR: No file system\n"));
    return NULL;
  }

  Status = Storage->FileSystem->OpenVolume (Storage->FileSystem, &Root);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: Invalid root volume - %r\n", Status));
    return NULL;
  }

  // TODO: What actually happens if we open/create here, and the file exists but is not a directory?
  Status = OcSafeFileOpen (
    Root,
    &NvramDir,
    OPEN_CORE_NVRAM_ROOT_PATH,
    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
    EFI_FILE_DIRECTORY
    );
  if (!EFI_ERROR (Status)) {
    Status = OcEnsureDirectoryFile (NvramDir, TRUE);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "VAR: %s found but not a directory - %r\n", OPEN_CORE_NVRAM_ROOT_PATH, Status));
    }

    NvramDir->Close (NvramDir);
    return NULL;
  }

  return NvramDir;
}

STATIC
EFI_STATUS
EFIAPI
LoadNvram (
  IN OC_STORAGE_CONTEXT               *Storage,
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

  DEBUG ((DEBUG_INFO, "VAR: Loading nvram...\n"));

  NvramDir = LocateNvramDir (Storage);
  if (NvramDir == NULL) {
    return EFI_NOT_FOUND;
  }

  FileBuffer = OcReadFileFromDirectory (NvramDir, OPEN_CORE_NVRAM_FILENAME, &FileSize, BASE_1MB);
  if (FileBuffer == NULL) {
    DEBUG ((DEBUG_INFO, "VAR: Trying fallback nvram data\n"));
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

  if (!IsValid || (NvramStorage.Version != OC_NVRAM_STORAGE_VERSION)) {
    DEBUG ((
      DEBUG_WARN,
      "VAR: Incompatible nvram data, version %u vs %u\n",
      NvramStorage.Version,
      OC_NVRAM_STORAGE_VERSION
      ));
    OC_NVRAM_STORAGE_DESTRUCT (&NvramStorage, sizeof (NvramStorage));
    return EFI_UNSUPPORTED;
  }

  for (GuidIndex = 0; GuidIndex < NvramStorage.Add.Count; ++GuidIndex) {
    Status = InternalProcessVariableGuid (
               OC_BLOB_GET (NvramStorage.Add.Keys[GuidIndex]),
               &VariableGuid,
               &NvramConfig->Legacy,
               &SchemaEntry
               );

    if (EFI_ERROR (Status)) {
      continue;
    }

    VariableMap = NvramStorage.Add.Values[GuidIndex];

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

STATIC
OC_PROCESS_VARIABLE_RESULT
EFIAPI
SerializeSectionVariables (
  IN EFI_GUID        *Guid,
  IN CHAR16          *Name,
  IN VOID            *Context
  )
{
  NVRAM_SAVE_CONTEXT      *SaveContext;
  UINT32                  Attributes;

  ASSERT (Context != NULL);
  SaveContext = Context;
  
  if (!CompareGuid (Guid, SaveContext->EntryGuid)) {
    return OcProcessVariableContinue;
  }

  // check the name in the SchemaEntry

  // Get the value, reallocating the space if needed
    Status = gRT->GetVariable (
                    Name,
                    Guid,
                    Attributes,
                    &DataSize,
                    CsrActiveConfig
                    );

  // Convert to base64, reallocating the space if needed

  // write
  
  return OcProcessVariableContinue;
}

STATIC
EFI_STATUS
EFIAPI
SaveNvram (
  IN OC_STORAGE_CONTEXT               *Storage,
  IN OC_NVRAM_CONFIG                  *NvramConfig
  )
{
  EFI_STATUS                    Status;
  EFI_FILE_PROTOCOL             *NvramDir;
  UINTN                         Index;
  UINT32                        GuidIndex;
  NVRAM_SAVE_CONTEXT            Context;

  DEBUG ((DEBUG_INFO, "VAR: Saving nvram...\n"));

  NvramDir = LocateNvramDir (Storage);
  if (NvramDir == NULL) {
      return EFI_NOT_FOUND;
  }

  Context.Status = EFI_SUCCESS;

  Context.ValueBuffer = ALLOCATE_POOL (BASE_1KB);
  if (Context.ValueBuffer == NULL) {
    NvramDir->Close (NvramDir);
    return EFI_OUT_OF_RESOURCES;
  }

  Context.Base64Buffer = ALLOCATE_POOL (BASE_1KB);
  if (Context.Base64Buffer == NULL) {
    NvramDir->Close (NvramDir);
    FreePool (Context.ValueBuffer);
    return EFI_OUT_OF_RESOURCES;
  }

  Context.StringBuffer = OcAsciiStringBufferInit ();
  if (Context.StringBuffer == NULL) {
    NvramDir->Close (NvramDir);
    FreePool (Context.ValueBuffer);
    FreePool (Context.Base64Buffer);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = OcAsciiStringBufferAppend (
    Context.StringBuffer,
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "        <key>Add</key>\n"
    "        <dict>\n"
    );

  if (EFI_ERROR (Status)) {
    NvramDir->Close (NvramDir);
    FreePool (Context.ValueBuffer);
    FreePool (Context.Base64Buffer);
    OcAsciiStringBufferFree (Context.StringBuffer);
    return Status;
  }

  for (GuidIndex = 0; GuidIndex < NvramConfig->Legacy.Count; ++GuidIndex) {
    Context.EntryGuid = OC_BLOB_GET (NvramConfig->Legacy.Keys[GuidIndex]);
    Context.SchemaEntry = NvramConfig->Legacy.Values[GuidIndex];

    Status = OcAsciiStringBufferSPrint (
      Context.StringBuffer,
      "                <key>%g</key>\n"
      "                <dict>\n",
      Context.EntryGuid
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
      "                </dict>\n"
      );

    if (EFI_ERROR (Status)) {
      break;
    }
  }

  FreePool (Context.ValueBuffer);
  FreePool (Context.Base64Buffer);

  if (!EFI_ERROR (Status)) {
    Status = OcAsciiStringBufferSPrint (
      Context.StringBuffer,
      "        </dict>\n"
      "        <key>Version</key>\n"
      "        <integer>%u</integer>\n"
      "</dict>\n"
      "</plist>\n",
      OC_NVRAM_STORAGE_VERSION);
  }

  if (EFI_ERROR (Status)) {
    NvramDir->Close (NvramDir);
    OcAsciiStringBufferFree (Context.StringBuffer);
    return Status;
  }

  Status = DeleteFile (NvramDir, OPEN_CORE_NVRAM_FILENAME);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: Error deleting %s - %r\n", OPEN_CORE_NVRAM_FILENAME, Status));
  }

  Status = OcSetFileData (
    NvramDir,
    OPEN_CORE_NVRAM_FILENAME,
    Context.StringBuffer,
    Context.StringBuffer->StringLength + 1
    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: Error saving %s - %r\n", OPEN_CORE_NVRAM_FILENAME, Status));
  }

  OcAsciiStringBufferFree (Context.StringBuffer);
  NvramDir->Close (NvramDir);

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
ResetNvram (
  IN OC_STORAGE_CONTEXT               *Storage,
  IN OC_NVRAM_CONFIG                  *NvramConfig
  )
{
  EFI_STATUS                    Status;
  EFI_STATUS                    AltStatus;
  EFI_FILE_PROTOCOL             *NvramDir;

  DEBUG ((DEBUG_INFO, "VAR: Resetting NVRAM...\n"));

  NvramDir = LocateNvramDir (Storage);
  if (NvramDir == NULL) {
    return EFI_NOT_FOUND;
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
  IN OC_STORAGE_CONTEXT               *Storage,
  IN OC_NVRAM_CONFIG                  *NvramConfig
  )
{
  EFI_STATUS                    Status;
  EFI_FILE_PROTOCOL             *NvramDir;
  EFI_FILE_PROTOCOL             *FallbackFile;
  EFI_FILE_PROTOCOL             *NvramFile;
  EFI_FILE_INFO                 *FileInfo;
  UINTN                         FileInfoSize;

  DEBUG ((DEBUG_INFO, "VAR: Switching to fallback NVRAM...\n"));

  NvramDir = LocateNvramDir (Storage);
  if (NvramDir == NULL) {
    return EFI_NOT_FOUND;
  }

  //
  // Do not do anything to main file if fallback file does not exist.
  //
  Status = OcSafeFileOpen (NvramDir, &FallbackFile, OPEN_CORE_NVRAM_FALLBACK_FILENAME, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: %s cannot be opened, not switching to fallback! - %r", OPEN_CORE_NVRAM_FALLBACK_FILENAME, Status));
    NvramDir->Close (NvramDir);
    return Status;
  }
  FallbackFile->Close (FallbackFile);
  
  Status = OcSafeFileOpen (NvramDir, &NvramFile, OPEN_CORE_NVRAM_FILENAME, EFI_FILE_MODE_READ, 0);
  NvramDir->Close (NvramDir);
  if (EFI_ERROR (Status)) {
    DEBUG ((Status == EFI_NOT_FOUND ? DEBUG_INFO : DEBUG_WARN, "VAR: %s cannot be opened, already switched to fallback! - %r", OPEN_CORE_NVRAM_FILENAME, Status));
    return Status == EFI_NOT_FOUND ? EFI_SUCCESS : Status;
  }

  FileInfoSize = 0;
  Status = NvramFile->GetInfo (NvramFile, &gEfiFileInfoGuid, &FileInfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_WARN, "VAR: %s cannot get file info size, not switching to fallback! - %r", OPEN_CORE_NVRAM_FILENAME, Status));
    return Status;
  }
  FileInfo = AllocatePool (FileInfoSize);
  if (FileInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = NvramFile->GetInfo (NvramFile, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_WARN, "VAR: %s cannot get file info, not switching to fallback! - %r", OPEN_CORE_NVRAM_FILENAME, Status));
    FreePool (FileInfo);
    return Status;
  }

  STATIC_ASSERT (L_STR_LEN (OPEN_CORE_NVRAM_USED_FILENAME) <= L_STR_LEN (OPEN_CORE_NVRAM_FILENAME), "NVRAM_USED_FILENAME length should be shorter than or equal to NVRAM_FILENAME length");
  StrCpyS (FileInfo->FileName, L_STR_SIZE (OPEN_CORE_NVRAM_USED_FILENAME), OPEN_CORE_NVRAM_USED_FILENAME);

  Status = NvramFile->SetInfo (NvramFile, &gEfiFileInfoGuid, FileInfoSize, FileInfo);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "VAR: Failure renaming %s -> %s - %r", OPEN_CORE_NVRAM_FILENAME, OPEN_CORE_NVRAM_USED_FILENAME, Status));
  }

  FreePool (FileInfo);
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
