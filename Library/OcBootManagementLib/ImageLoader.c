/** @file
  Copyright (C) 2019, vit9696. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include "BootManagementInternal.h"

#include <Protocol/DevicePath.h>
#include <Protocol/HotPlugDevice.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/UserInterfaceTheme.h>
#include <Protocol/UgaDraw.h>

#include <Protocol/ConsoleControl.h>
#include <Protocol/SimplePointer.h>
#include <Library/OcDirectResetLib.h>
#include <Protocol/AppleFramebufferInfo.h>
#include <Protocol/AppleEg2Info.h>
#include <Protocol/AppleGraphicsPolicy.h>
#include <Protocol/AppleUserInterface.h>

#include <Guid/AppleVariable.h>
#include <Guid/DxeServices.h>
#include <Guid/FileInfo.h>
#include <Guid/GlobalVariable.h>
#include <Guid/OcVariable.h>
#include <Guid/ConsoleOutDevice.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAppleSecureBootLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/OcDevicePathLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcMachoLib.h>
#include <Library/OcMiscLib.h>
#include <Library/OcStringLib.h>
#include <Library/OcPeCoffLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PrintLib.h>

#include <Library/OcTimerLib.h>

#if defined (MDE_CPU_IA32)
#define OC_IMAGE_FILE_MACHINE  IMAGE_FILE_MACHINE_I386
#elif defined (MDE_CPU_X64)
#define OC_IMAGE_FILE_MACHINE  IMAGE_FILE_MACHINE_X64
#else
  #error Unsupported architecture.
#endif

STATIC EFI_GUID  mOcLoadedImageProtocolGuid = {
  0x1f3c963d, 0xf9dc, 0x4537, { 0xbb, 0x06, 0xd8, 0x08, 0x46, 0x4a, 0x85, 0x2e }
};

typedef struct {
  EFI_IMAGE_ENTRY_POINT        EntryPoint;
  EFI_PHYSICAL_ADDRESS         ImageArea;
  UINTN                        PageCount;
  EFI_STATUS                   Status;
  VOID                         *JumpBuffer;
  BASE_LIBRARY_JUMP_BUFFER     *JumpContext;
  CHAR16                       *ExitData;
  UINTN                        ExitDataSize;
  UINT16                       Subsystem;
  BOOLEAN                      Started;
  EFI_LOADED_IMAGE_PROTOCOL    LoadedImage;
} OC_LOADED_IMAGE_PROTOCOL;

STATIC EFI_IMAGE_LOAD    mOriginalEfiLoadImage;
STATIC EFI_IMAGE_START   mOriginalEfiStartImage;
STATIC EFI_IMAGE_UNLOAD  mOriginalEfiUnloadImage;
STATIC EFI_EXIT          mOriginalEfiExit;
STATIC EFI_HANDLE        mCurrentImageHandle;

STATIC OC_IMAGE_LOADER_PATCH      mImageLoaderPatch;
STATIC OC_IMAGE_LOADER_CONFIGURE  mImageLoaderConfigure;
STATIC UINT32                     mImageLoaderCaps;
STATIC BOOLEAN                    mImageLoaderEnabled;

STATIC BOOLEAN  mProtectUefiServices;

STATIC EFI_IMAGE_LOAD          mPreservedLoadImage;
STATIC EFI_IMAGE_START         mPreservedStartImage;
STATIC EFI_EXIT_BOOT_SERVICES  mPreservedExitBootServices;
STATIC EFI_EXIT                mPreservedExit;

STATIC USER_INTERFACE_CONNECT_GOP mOriginalConnectGop;

STATIC USER_INTERFACE_CREATE_DRAW_BUFFER mOriginalCreateDrawBuffer;
STATIC USER_INTERFACE_FREE_DRAW_BUFFER mOriginalFreeDrawBuffer;

STATIC APPLE_FIRMWARE_USER_INTERFACE_PROTOCOL  *mUIProtocol;
//STATIC APPLE_FIRMWARE_UI_VARS                        *mUIVars;

STATIC EFI_DXE_SERVICES *mDS = NULL;

STATIC
EFI_STATUS
LocateDxeServicesTable (
  VOID
  )
{
  EFI_STATUS Status;
  UINTN Index;

  if (mDS != NULL) {
    return EFI_SUCCESS;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if (CompareGuid(&gEfiDxeServicesTableGuid, &gST->ConfigurationTable[Index].VendorGuid)) {
      mDS = gST->ConfigurationTable[Index].VendorTable;
      Status = EFI_SUCCESS;
      break;
    }
  }

  return Status;
}

EFI_STATUS
OcGetAppleFirmwareUI (
  IN APPLE_FIRMWARE_USER_INTERFACE_PROTOCOL  **UIProtocol,
  IN APPLE_FIRMWARE_UI_VARS                  **UIVars OPTIONAL
  )
{
  EFI_STATUS                              Status;

  ASSERT (UIProtocol != NULL);

  Status = gBS->LocateProtocol (
    &gAppleFirmwareUserInterfaceProtocolGuid,
    NULL,
    (VOID **)UIProtocol
  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCB: Cannot locate firmware UI protocol - %r\n", Status));
    return Status;
  }

  if (UIVars != NULL) {
    *UIVars = (VOID *)((UINT8 *)*UIProtocol + sizeof (APPLE_FIRMWARE_USER_INTERFACE_PROTOCOL));
  }

  return Status;
}

STATIC
VOID
PreserveGrubShimHooks (
  VOID
  )
{
  if (!mProtectUefiServices) {
    return;
  }

  mPreservedLoadImage        = gBS->LoadImage;
  mPreservedStartImage       = gBS->StartImage;
  mPreservedExitBootServices = gBS->ExitBootServices;
  mPreservedExit             = gBS->Exit;
}

//
// REF: https://github.com/acidanthera/bugtracker/issues/1874
//
STATIC
VOID
RestoreGrubShimHooks (
  IN CONST CHAR8  *Caller
  )
{
  if (!mProtectUefiServices) {
    return;
  }

  if ((gBS->LoadImage        != mPreservedLoadImage) ||
      (gBS->StartImage       != mPreservedStartImage) ||
      (gBS->ExitBootServices != mPreservedExitBootServices) ||
      (gBS->Exit             != mPreservedExit))
  {
    DEBUG ((
      DEBUG_INFO,
      "OCB: Restoring trashed L:%u S:%u EBS:%u E:%u after %a\n",
      gBS->LoadImage        != mPreservedLoadImage,
      gBS->StartImage       != mPreservedStartImage,
      gBS->ExitBootServices != mPreservedExitBootServices,
      gBS->Exit             != mPreservedExit,
      Caller
      ));

    gBS->LoadImage        = mPreservedLoadImage;
    gBS->StartImage       = mPreservedStartImage;
    gBS->ExitBootServices = mPreservedExitBootServices;
    gBS->Exit             = mPreservedExit;
  }
}

STATIC
EFI_STATUS
InternalEfiLoadImageFile (
  IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  OUT UINTN                     *FileSize,
  OUT VOID                      **FileBuffer
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  VOID               *Buffer;
  UINT32             Size;

  Status = OcOpenFileByDevicePath (
             &DevicePath,
             &File,
             EFI_FILE_MODE_READ,
             0
             );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = OcGetFileSize (
             File,
             &Size
             );
  if (EFI_ERROR (Status) || (Size == 0)) {
    File->Close (File);
    return EFI_UNSUPPORTED;
  }

  Buffer = AllocatePool (Size);
  if (Buffer == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = OcGetFileData (
             File,
             0,
             Size,
             Buffer
             );
  if (EFI_ERROR (Status)) {
    FreePool (Buffer);
    File->Close (File);
    return EFI_DEVICE_ERROR;
  }

  *FileBuffer = Buffer;
  *FileSize   = Size;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
InternalEfiLoadImageProtocol (
  IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN  BOOLEAN                   UseLoadImage2,
  OUT UINTN                     *FileSize,
  OUT VOID                      **FileBuffer
  )
{
  //
  // TODO: Implement image load protocol if necessary.
  //
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
InternalUpdateLoadedImage (
  IN EFI_HANDLE                ImageHandle,
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 DeviceHandle;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL   *RemainingDevicePath;

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  RemainingDevicePath = DevicePath;
  Status              = gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &RemainingDevicePath, &DeviceHandle);
  if (EFI_ERROR (Status)) {
    //
    // TODO: Handle load protocol if necessary.
    //
    return Status;
  }

  if (LoadedImage->DeviceHandle != DeviceHandle) {
    LoadedImage->DeviceHandle = DeviceHandle;
    LoadedImage->FilePath     = DuplicateDevicePath (RemainingDevicePath);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
OcImageLoaderLoad (
  IN  BOOLEAN                   BootPolicy,
  IN  EFI_HANDLE                ParentImageHandle,
  IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN  VOID                      *SourceBuffer OPTIONAL,
  IN  UINTN                     SourceSize,
  OUT EFI_HANDLE                *ImageHandle
  )
{
  EFI_STATUS                 Status;
  EFI_STATUS                 ImageStatus;
  PE_COFF_IMAGE_CONTEXT      ImageContext;
  EFI_PHYSICAL_ADDRESS       DestinationArea;
  UINT32                     DestinationSize;
  VOID                       *DestinationBuffer;
  OC_LOADED_IMAGE_PROTOCOL   *OcLoadedImage;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;

  ASSERT (SourceBuffer != NULL);

  //
  // Reject very large files.
  //
  if (SourceSize > MAX_UINT32) {
    return EFI_UNSUPPORTED;
  }

  //
  // Initialize the image context.
  //
  ImageStatus = PeCoffInitializeContext (
                  &ImageContext,
                  SourceBuffer,
                  (UINT32)SourceSize
                  );
  if (EFI_ERROR (ImageStatus)) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff init failure - %r\n", ImageStatus));
    return EFI_UNSUPPORTED;
  }

  //
  // Reject images that are not meant for the platform's architecture.
  //
  if (ImageContext.Machine != OC_IMAGE_FILE_MACHINE) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff wrong machine - %x\n", ImageContext.Machine));
    return EFI_UNSUPPORTED;
  }

  //
  // Reject RT drivers for the moment.
  //
  if (ImageContext.Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff no support for RT drivers\n"));
    return EFI_UNSUPPORTED;
  }

  //
  // FIXME: This needs to be backported as a function:
  // https://github.com/mhaeuser/edk2/blob/2021-gsoc-secure-loader/MdePkg/Library/BaseUefiImageLib/CommonSupport.c#L19-L53
  //
  DestinationSize = ImageContext.SizeOfImage + ImageContext.SizeOfImageDebugAdd;
  if (OcOverflowAddU32 (DestinationSize, ImageContext.SectionAlignment, &DestinationSize)) {
    return RETURN_UNSUPPORTED;
  }

  if (DestinationSize >= BASE_16MB) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff prohibits files over 16M (%u)\n", DestinationSize));
    return RETURN_UNSUPPORTED;
  }

  //
  // Allocate the image destination memory.
  // FIXME: RT drivers require EfiRuntimeServicesCode.
  //
  Status = gBS->AllocatePages (
                  AllocateAnyPages,
                  ImageContext.Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION
      ? EfiLoaderCode : EfiBootServicesCode,
                  EFI_SIZE_TO_PAGES (ImageContext.SizeOfImage),
                  &DestinationArea
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DestinationBuffer = (VOID *)(UINTN)DestinationArea;

  //
  // Load SourceBuffer into DestinationBuffer.
  //
  ImageStatus = PeCoffLoadImage (
                  &ImageContext,
                  DestinationBuffer,
                  ImageContext.SizeOfImage
                  );
  if (EFI_ERROR (ImageStatus)) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff load image error - %r\n", ImageStatus));
    FreePages (DestinationBuffer, EFI_SIZE_TO_PAGES (ImageContext.SizeOfImage));
    return EFI_UNSUPPORTED;
  }

  //
  // Relocate the loaded image to the destination address.
  //
  ImageStatus = PeCoffRelocateImage (
                  &ImageContext,
                  (UINTN)DestinationBuffer,
                  NULL,
                  0
                  );
  if (EFI_ERROR (ImageStatus)) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff relocate image error - %r\n", ImageStatus));
    FreePages (DestinationBuffer, EFI_SIZE_TO_PAGES (ImageContext.SizeOfImage));
    return EFI_UNSUPPORTED;
  }

  //
  // Construct a LoadedImage protocol for the image.
  //
  OcLoadedImage = AllocateZeroPool (sizeof (*OcLoadedImage));
  if (OcLoadedImage == NULL) {
    FreePages (DestinationBuffer, EFI_SIZE_TO_PAGES (ImageContext.SizeOfImage));
    return EFI_OUT_OF_RESOURCES;
  }

  OcLoadedImage->EntryPoint = (EFI_IMAGE_ENTRY_POINT)((UINTN)DestinationBuffer + ImageContext.AddressOfEntryPoint);
  OcLoadedImage->ImageArea  = DestinationArea;
  OcLoadedImage->PageCount  = EFI_SIZE_TO_PAGES (ImageContext.SizeOfImage);
  OcLoadedImage->Subsystem  = ImageContext.Subsystem;

  LoadedImage = &OcLoadedImage->LoadedImage;

  LoadedImage->Revision     = EFI_LOADED_IMAGE_INFORMATION_REVISION;
  LoadedImage->ParentHandle = ParentImageHandle;
  LoadedImage->SystemTable  = gST;
  LoadedImage->ImageBase    = DestinationBuffer;
  LoadedImage->ImageSize    = ImageContext.SizeOfImage;
  //
  // FIXME: Support RT drivers.
  //
  if (ImageContext.Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION) {
    LoadedImage->ImageCodeType = EfiLoaderCode;
    LoadedImage->ImageDataType = EfiLoaderData;
  } else {
    LoadedImage->ImageCodeType = EfiBootServicesCode;
    LoadedImage->ImageDataType = EfiBootServicesData;
  }

  //
  // Install LoadedImage and the image's entry point.
  //
  *ImageHandle = NULL;
  Status       = gBS->InstallMultipleProtocolInterfaces (
                        ImageHandle,
                        &gEfiLoadedImageProtocolGuid,
                        LoadedImage,
                        &mOcLoadedImageProtocolGuid,
                        OcLoadedImage,
                        NULL
                        );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff proto install error - %r\n", Status));
    FreePool (OcLoadedImage);
    FreePages (DestinationBuffer, EFI_SIZE_TO_PAGES (ImageContext.SizeOfImage));
    return Status;
  }

  DEBUG ((DEBUG_VERBOSE, "OCB: Loaded image at %p\n", *ImageHandle));

  return EFI_SUCCESS;
}

/**
  Unload image routine for OcImageLoaderLoad.

  @param[in]  OcLoadedImage     Our loaded image instance.
  @param[in]  ImageHandle       Handle that identifies the image to be unloaded.

  @retval EFI_SUCCESS           The image has been unloaded.
**/
STATIC
EFI_STATUS
InternalDirectUnloadImage (
  IN  OC_LOADED_IMAGE_PROTOCOL  *OcLoadedImage,
  IN  EFI_HANDLE                ImageHandle
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;

  LoadedImage = &OcLoadedImage->LoadedImage;
  if (LoadedImage->Unload != NULL) {
    Status = LoadedImage->Unload (ImageHandle);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    //
    // Do not allow to execute Unload multiple times.
    //
    LoadedImage->Unload = NULL;
  } else if (OcLoadedImage->Started) {
    return EFI_UNSUPPORTED;
  }

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  LoadedImage,
                  &mOcLoadedImageProtocolGuid,
                  OcLoadedImage,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  gBS->FreePages (OcLoadedImage->ImageArea, OcLoadedImage->PageCount);
  FreePool (OcLoadedImage);
  //
  // NOTE: Avoid EFI 1.10 extension of closing opened protocols.
  //
  return EFI_SUCCESS;
}

/**
  Unload image routine for OcImageLoaderLoad.

  @param[in]  OcLoadedImage     Our loaded image instance.
  @param[in]  ImageHandle       Handle that identifies the image to be unloaded.
  @param[in]  ExitStatus        The image's exit code.
  @param[in]  ExitDataSize      The size, in bytes, of ExitData. Ignored if ExitStatus is EFI_SUCCESS.
  @param[in]  ExitData          The pointer to a data buffer that includes a Null-terminated string,
                                optionally followed by additional binary data. The string is a
                                description that the caller may use to further indicate the reason
                                for the image's exit. ExitData is only valid if ExitStatus
                                is something other than EFI_SUCCESS. The ExitData buffer
                                must be allocated by calling AllocatePool().

  @retval EFI_SUCCESS           The image has been unloaded.
**/
STATIC
EFI_STATUS
InternalDirectExit (
  IN  OC_LOADED_IMAGE_PROTOCOL  *OcLoadedImage,
  IN  EFI_HANDLE                ImageHandle,
  IN  EFI_STATUS                ExitStatus,
  IN  UINTN                     ExitDataSize,
  IN  CHAR16                    *ExitData     OPTIONAL
  )
{
  EFI_TPL  OldTpl;

  DEBUG ((
    DEBUG_VERBOSE,
    "OCB: Exit %p %p (%d) - %r\n",
    ImageHandle,
    mCurrentImageHandle,
    OcLoadedImage->Started,
    ExitStatus
    ));

  //
  // Prevent possible reentrance to this function for the same ImageHandle.
  //
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);

  //
  // If the image has not been started just free its resources.
  // Should not happen normally.
  //
  if (!OcLoadedImage->Started) {
    InternalDirectUnloadImage (OcLoadedImage, ImageHandle);
    gBS->RestoreTPL (OldTpl);
    return EFI_SUCCESS;
  }

  //
  // If the image has been started, verify this image can exit.
  //
  if (ImageHandle != mCurrentImageHandle) {
    DEBUG ((DEBUG_LOAD|DEBUG_ERROR, "OCB: Image is not exitable image\n"));
    gBS->RestoreTPL (OldTpl);
    return EFI_INVALID_PARAMETER;
  }

  //
  // Set the return status.
  //
  OcLoadedImage->Status = ExitStatus;

  //
  // If there's ExitData info provide it.
  //
  if (ExitData != NULL) {
    OcLoadedImage->ExitDataSize = ExitDataSize;
    OcLoadedImage->ExitData     = AllocatePool (OcLoadedImage->ExitDataSize);
    if (OcLoadedImage->ExitData != NULL) {
      CopyMem (OcLoadedImage->ExitData, ExitData, OcLoadedImage->ExitDataSize);
    } else {
      OcLoadedImage->ExitDataSize = 0;
    }
  }

  //
  // return to StartImage
  //
  gBS->RestoreTPL (OldTpl);
  LongJump (OcLoadedImage->JumpContext, (UINTN)-1);

  //
  // If we return from LongJump, then it is an error
  //
  ASSERT (FALSE);
  CpuDeadLoop ();
  return EFI_ACCESS_DENIED;
}

EFI_CONSOLE_CONTROL_PROTOCOL_SET_MODE *mOriginalSetModePos = NULL;
EFI_CONSOLE_CONTROL_PROTOCOL_SET_MODE mOriginalSetMode;

EFI_CONSOLE_CONTROL_PROTOCOL *
GetAndDebugConsoleControl (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_CONSOLE_CONTROL_PROTOCOL     *ConsoleControl;
  EFI_CONSOLE_CONTROL_SCREEN_MODE  Mode;
  BOOLEAN                          GopUgaExists;
  BOOLEAN                          StdInLocked;

  Status = gBS->LocateProtocol (
                  &gEfiConsoleControlProtocolGuid,
                  NULL,
                  (VOID **)&ConsoleControl
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "WRAP: Cannot locate ConsoleControl protocol - %r\n", Status));
    return NULL;
  }

  Status = ConsoleControl->GetMode (ConsoleControl, &Mode, &GopUgaExists, &StdInLocked);
  DEBUG ((DEBUG_INFO, "WRAP: ConsoleControl interface @ %p GetMode = %u %u %u - %r\n", ConsoleControl, Mode, GopUgaExists, StdInLocked, Status));

  return ConsoleControl;
}

EFI_STATUS
EFIAPI
WrappedSetMode (
  IN EFI_CONSOLE_CONTROL_PROTOCOL     *This,
  IN EFI_CONSOLE_CONTROL_SCREEN_MODE  Mode
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN Nested = FALSE;

  if (Nested) {
    return mOriginalSetMode (This, Mode);
  }

  Nested = TRUE;
  DEBUG ((DEBUG_INFO, "WRAP: -> SetMode %u\n", Mode));
  GetAndDebugConsoleControl ();
  Status = mOriginalSetMode (This, Mode);
#if 0
  // report original status, but fake success anyway
  DEBUG ((DEBUG_INFO, "WRAP: <- SetMode %u - %r/%r\n", Mode, Status, EFI_SUCCESS));
  Status = EFI_SUCCESS;
#else
  DEBUG ((DEBUG_INFO, "WRAP: <- SetMode %u - %r\n", Mode, Status));
#endif
  Nested = FALSE;

  return Status;
}

VOID
WrapSetMode (
  VOID
  )
{
  EFI_CONSOLE_CONTROL_PROTOCOL     *ConsoleControl;

  ConsoleControl = GetAndDebugConsoleControl ();

  if (ConsoleControl == NULL) {
    return;
  }

  mOriginalSetModePos = &ConsoleControl->SetMode;
  mOriginalSetMode = ConsoleControl->SetMode;
  ConsoleControl->SetMode = WrappedSetMode;
}

VOID
UnwrapSetMode (
  VOID
  )
{
  if (mOriginalSetModePos != NULL) {
    *mOriginalSetModePos = mOriginalSetMode;
  }
}

STATIC
EFI_GUID *mBlockGuids[] = {
  // //&gAppleFramebufferInfoProtocolGuid,
  // &gAppleUserInterfaceThemeProtocolGuid,
  // &gEfiGraphicsOutputProtocolGuid,

  // &gAppleEg2InfoProtocolGuid,
  // &gEfiUgaDrawProtocolGuid,
  // &gAppleFramebufferInfoProtocolGuid,
  // &gAppleGraphicsPolicyProtocolGuid,
  // &gEfiGraphicsOutputProtocolGuid
};

BOOLEAN
BlockEm (
  EFI_GUID *Protocol,
  EFI_STATUS *Status,
  BOOLEAN Nested
  )
{
  UINTN Index;

  if (Protocol == NULL) {
    return FALSE;
  }

  for (Index = 0; Index < ARRAY_SIZE (mBlockGuids); Index++) {
    if (CompareGuid (Protocol, mBlockGuids[Index])) {
      if (Nested) {
        DirectResetCold ();
      }

      *Status = EFI_NOT_FOUND;
      DEBUG ((DEBUG_INFO, "BLOCK: %g - %r\n", Protocol, *Status));
      return TRUE;
    }
  }

  return FALSE;
}

STATIC BOOLEAN mLogAllocate;

STATIC EFI_ALLOCATE_POOL mOriginalAllocatePool;
STATIC EFI_FREE_POOL mOriginalFreePool;
STATIC EFI_SET_MEM mOriginalSetMem;

typedef enum {
  TrapAllocatePool,
  TrapFreePool,
  TrapSetMem
} ALLOC_TRAP_TYPE;

typedef struct {
  ALLOC_TRAP_TYPE              TrapType;
  EFI_MEMORY_TYPE              PoolType;
  UINTN                        Size;
  VOID                         *Buffer;
  CHAR8                        Value;
  EFI_STATUS                   Status;
} ALLOC_TRAP_INFO;

VOID
Sleep (
  IN UINT64 Timeout
  )
{
  UINT64           EndTime;
  UINT64           CurrTime;

  if (Timeout == 0) {
    EndTime = 0ULL;
  } else {
    EndTime = GetPerformanceCounter ();
    if (EndTime != 0) {
      EndTime = GetTimeInNanoSecond (EndTime) + Timeout * 1000000ULL;
    }
  }

  if (EndTime != 0) {
    do {
      CurrTime = GetTimeInNanoSecond (GetPerformanceCounter ());
    }
    while ((CurrTime != 0) && (CurrTime < EndTime));
  }
}

VOID
DumpTrapInfo (
  ALLOC_TRAP_TYPE              TrapType,
  EFI_MEMORY_TYPE              PoolType,
  UINTN                        Size,
  VOID                         *Buffer,
  CHAR8                        Value,
  EFI_STATUS                   Status
  )
{
  ALLOC_TRAP_INFO TrapInfo;
  CHAR16          DumpName[] = L"trapN";
  CHAR16          DumpCh;
  STATIC UINTN    DumpIndex = 0;

  TrapInfo.TrapType = TrapType;
  TrapInfo.PoolType = PoolType;
  TrapInfo.Size = Size;
  TrapInfo.Buffer = Buffer;
  TrapInfo.Value = Value;
  TrapInfo.Status = Status;

  if (DumpIndex == 16) {
    Sleep (10 * 1000);
    DirectResetCold ();
  }

  if (DumpIndex < 10) {
    DumpCh = '0' + DumpIndex;
  } else {
    DumpCh = 'A' - 10 + DumpIndex;
  }
  DumpIndex = (DumpIndex + 1) % 36;

  DumpName[L_STR_LEN (DumpName) - 1] = DumpCh;

  gRT->SetVariable (
                  DumpName,
                  &gAppleBootVariableGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                  sizeof (TrapInfo),
                  &TrapInfo
                  );
}

EFI_STATUS
EFIAPI WrappedAllocatePool (
  IN  EFI_MEMORY_TYPE              PoolType,
  IN  UINTN                        Size,
  OUT VOID                         **Buffer
  )
{
  EFI_STATUS      Status;
  STATIC BOOLEAN  Nested = FALSE;

  if (Nested) {
    return mOriginalAllocatePool (PoolType, Size, Buffer);
  }

  if (mLogAllocate) {
    DumpTrapInfo (TrapAllocatePool, PoolType, Size, NULL, 0, MAX_UINT64);
    // DEBUG ((DEBUG_INFO, "WRAP: AllocatePool t=%u s=%u *b%a=%p - %r\n", PoolType, Size, Buffer == NULL ? "<null>" : "", Buffer == NULL ? NULL : *Buffer, Status));
  }

  Nested = TRUE;
  Status = mOriginalAllocatePool (PoolType, Size, Buffer);
  Nested = FALSE;

  if (mLogAllocate) {
    DumpTrapInfo (TrapAllocatePool, PoolType, Size, Buffer == NULL ? NULL : *Buffer, 0, Status);
    // DEBUG ((DEBUG_INFO, "WRAP: AllocatePool t=%u s=%u *b%a=%p - %r\n", PoolType, Size, Buffer == NULL ? "<null>" : "", Buffer == NULL ? NULL : *Buffer, Status));
  }

  return Status;
}

EFI_STATUS
EFIAPI WrappedFreePool (
  IN  VOID                         *Buffer
  )
{
  EFI_STATUS      Status;
  STATIC BOOLEAN  Nested = FALSE;

  if (Nested) {
    return mOriginalFreePool (Buffer);
  }

  Nested = TRUE;
  Status = mOriginalFreePool (Buffer);
  Nested = FALSE;

  if (mLogAllocate) {
    DumpTrapInfo (TrapFreePool, 0, 0, Buffer, 0, Status);
    // DEBUG ((DEBUG_INFO, "WRAP: FreePool b=%p - %r\n", Buffer, Status));
  }

  return Status;
}

VOID
EFIAPI WrappedSetMem (
  IN VOID     *Buffer,
  IN UINTN    Size,
  IN UINT8    Value
  )
{
  STATIC BOOLEAN  Nested = FALSE;

  if (Nested) {
    mOriginalSetMem (Buffer, Size, Value);
    return;
  }

  Nested = TRUE;
  mOriginalSetMem (Buffer, Size, Value);
  Nested = FALSE;

  if (mLogAllocate) {
    DumpTrapInfo (TrapSetMem, 0, Size, Buffer, Value, 0);
    // DEBUG ((DEBUG_INFO, "WRAP: SetMem b=%p s=%u v=0x%02x\n", Buffer, Size, Value));
  }
}

//STATIC
VOID
WrapAllocate (
  IN EFI_BOOT_SERVICES *lBS
  )
{
  mOriginalAllocatePool = lBS->AllocatePool;
  lBS->AllocatePool = WrappedAllocatePool;

  mOriginalFreePool = lBS->FreePool;
  lBS->FreePool = WrappedFreePool;

  mOriginalSetMem = lBS->SetMem;
  lBS->SetMem = WrappedSetMem;
}

//STATIC
VOID
UnwrapAllocate (
  IN EFI_BOOT_SERVICES *lBS
  )
{
  lBS->AllocatePool = mOriginalAllocatePool;
  lBS->FreePool = mOriginalFreePool;
  lBS->SetMem = mOriginalSetMem;
}

typedef
VOID *
(EFIAPI *ALLOCATE_ZERO_POOL)(
  IN UINTN  AllocationSize
  );

EFI_SYSTEM_TABLE      *aST;
EFI_BOOT_SERVICES     *aBS;
EFI_RUNTIME_SERVICES  *aRT;

//STATIC
EFI_GUID gEfiUnusedGuid = { 0x00000000, 0x0000, 0x0000, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }};

extern
//STATIC
EFI_STATUS
EFIAPI
WrappedLocateProtocol (
  IN  EFI_GUID  *Protocol,
  IN  VOID      *Registration OPTIONAL,
  OUT VOID      **Interface
  );

extern
EFI_STATUS
DumpGop (
  EFI_GRAPHICS_OUTPUT_PROTOCOL    *Gop,
  CHAR8                           *GopFriendlyName
  );

extern
EFI_STATUS
DumpGopForHandle (
  EFI_HANDLE    Handle,
  CHAR8         *HandleFriendlyName,
  CHAR8         *GopFriendlyName
  );

VOID
ShowAppleGop (
  APPLE_FIRMWARE_USER_INTERFACE_PROTOCOL *UIProtocol
  )
{
  UINT8                         *aGopAlreadyConnected;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  **aGop;

  aGopAlreadyConnected = (VOID *)((UINT8 *)UIProtocol + sizeof (APPLE_FIRMWARE_USER_INTERFACE_PROTOCOL));
  aGop = (VOID *)((UINT8 *)UIProtocol + sizeof (APPLE_FIRMWARE_USER_INTERFACE_PROTOCOL) +0x8);

  DEBUG ((DEBUG_INFO, "DUMP: aGop %p, aGopAlreadyConnected %u\n", *aGop, *aGopAlreadyConnected));
  DumpGop (*aGop, "aGop");
}

EFI_STATUS
EFIAPI
WrappedConnectGop (
  VOID
  )
{
  EFI_STATUS Status;

  DEBUG ((DEBUG_INFO, "WRAP: -> ConnectGop\n"));

  // ShowAppleGop (mUIProtocol);

  Status = mOriginalConnectGop ();

  // ShowAppleGop (mUIProtocol);
  
  DEBUG ((DEBUG_INFO, "WRAP: <- ConnectGop - %r\n", Status));

  return Status;
}

VOID
WrapConnectGop (
  APPLE_FIRMWARE_USER_INTERFACE_PROTOCOL  *UIProtocol
  )
{
  mOriginalConnectGop = UIProtocol->ConnectGop;
  UIProtocol->ConnectGop = WrappedConnectGop;
}

VOID
UnwrapConnectGop (
  APPLE_FIRMWARE_USER_INTERFACE_PROTOCOL  *UIProtocol
  )
{
  UIProtocol->ConnectGop = mOriginalConnectGop;
}

EFI_STATUS
EFIAPI WrappedCreateDrawBuffer (
  OUT VOID    *DrawBufferInfo,
  IN UINT32   BackgroundColor
  )
{
  EFI_STATUS          Status;

  DEBUG ((DEBUG_INFO, "WRAP: -> CreateDrawBuffer\n"));

  // OcSetFileData (NULL, L"CreateDrawBuffer.bin", mOriginalCreateDrawBuffer, 0x180);

  // mLogAllocate = TRUE;
  // WrapAllocate (aBS);

  Status = mOriginalCreateDrawBuffer (DrawBufferInfo, BackgroundColor);

  // UnwrapAllocate (aBS);
  // mLogAllocate = FALSE;

  DEBUG ((DEBUG_INFO, "WRAP: <- CreateDrawBuffer - %r\n", Status));
  return Status;
}

VOID
EFIAPI WrappedFreeDrawBuffer (
  IN VOID     *DrawBufferInfo
  )
{
#if 0
  DirectResetCold ();
#else
  DEBUG ((DEBUG_INFO, "WRAP: -> FreeDrawBuffer\n"));
  mOriginalFreeDrawBuffer (DrawBufferInfo);
  DEBUG ((DEBUG_INFO, "WRAP: <- FreeDrawBuffer\n"));
#endif
}

STATIC
VOID
WrapDrawBuffer (
  APPLE_FIRMWARE_USER_INTERFACE_PROTOCOL  *UIProtocol
  )
{
  mOriginalCreateDrawBuffer = UIProtocol->CreateDrawBuffer;
  UIProtocol->CreateDrawBuffer = WrappedCreateDrawBuffer;

  mOriginalFreeDrawBuffer = UIProtocol->FreeDrawBuffer;
  UIProtocol->FreeDrawBuffer = WrappedFreeDrawBuffer;
}

STATIC
VOID
UnwrapDrawBuffer (
  APPLE_FIRMWARE_USER_INTERFACE_PROTOCOL  *UIProtocol
  )
{
    UIProtocol->FreeDrawBuffer = mOriginalFreeDrawBuffer;
    UIProtocol->CreateDrawBuffer = mOriginalCreateDrawBuffer;
}

STATIC BOOLEAN mAllowLogConnect = FALSE;
STATIC UINTN ConnectControllerNesting = 0;

typedef
EFI_STATUS
(EFIAPI *EFI_DISPATCH)(
  VOID
  );

STATIC EFI_CONNECT_CONTROLLER mOriginalConnectController;
STATIC EFI_LOCATE_PROTOCOL mOriginalLocateProtocol;
STATIC EFI_LOCATE_HANDLE_BUFFER mOriginalLocateHandleBuffer;
STATIC EFI_OPEN_PROTOCOL mOriginalOpenProtocol;
STATIC EFI_DISPATCH mOriginalDispatch;

EFI_STATUS
HotPlugGop (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       HandleCount;
  EFI_HANDLE  *HandleBuffer;
  UINTN       Index;

  Status      = gBS->LocateHandleBuffer (
                       ByProtocol,
                       &gEfiGraphicsOutputProtocolGuid,
                       NULL,
                       &HandleCount,
                       &HandleBuffer
                       );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;

  for (Index = 0; Index < HandleCount; ++Index) {
    if (HandleBuffer[Index] != gST->ConsoleOutHandle) {
      Status = gBS->InstallProtocolInterface (
        &HandleBuffer[Index],
        &gEfiHotPlugDeviceGuid,
        EFI_NATIVE_INTERFACE,
        NULL
      );
      DEBUG ((DEBUG_INFO, "WRAP: Make handle %p hot plug - %r\n", HandleBuffer[Index], Status));
    }
  }

  return Status;
}

//STATIC
EFI_STATUS
EFIAPI
WrappedDispatch (
  VOID
  )
{
  EFI_STATUS        Status;
  STATIC UINTN      Nesting = 0;
  STATIC UINTN      Count = 1; //////// 0;

  DEBUG ((DEBUG_INFO, "WRAP: -> gDS->Dispatch n = %u\n", Nesting));

  Nesting++;
  Status = mOriginalDispatch ();
  Nesting--;

  DEBUG ((DEBUG_INFO, "WRAP: <- gDS->Dispatch n = %u - %r\n", Nesting, Status));

  if (Count == 0) {
    DEBUG ((DEBUG_INFO, "WRAP: Forcing connect...\n"));
    mAllowLogConnect = FALSE;
    UsefulDump ("POST-DISPATCH-PRE-FORCE");
    mAllowLogConnect = TRUE;

    HotPlugGop ();

    // // mUIProtocol->ConnectGop ();

    // Status = gBS->InstallProtocolInterface (
    //   &gST->ConsoleOutHandle,
    //   &gEfiConsoleOutDeviceGuid,
    //   EFI_NATIVE_INTERFACE,
    //   NULL);

    // DEBUG ((
    //   DEBUG_INFO,
    //   "WRAP: Installing protocol %g onto handle %p - %r\n",
    //   &gEfiConsoleOutDeviceGuid,
    //   gST->ConsoleOutHandle,
    //   Status
    // ));

    mAllowLogConnect = FALSE;
    UsefulDump ("POST-DISPATCH-POST-FORCE");
    mAllowLogConnect = TRUE;
    Status = EFI_SUCCESS;  // force re-scan
    
    Count++;
  } else {
    mAllowLogConnect = FALSE;
    UsefulDump ("POST-DISPATCH");
  }

  return Status;
}

STATIC
VOID
WrapDispatch (
  EFI_DXE_SERVICES *lDS
  )
{
  mOriginalDispatch = lDS->Dispatch;
  lDS->Dispatch = WrappedDispatch;
}

STATIC
VOID
UnwrapDispatch (
  EFI_DXE_SERVICES *lDS
  )
{
  lDS->Dispatch = mOriginalDispatch;
  mOriginalDispatch = NULL;
}

//STATIC
EFI_STATUS
EFIAPI
WrappedConnectController (
  IN  EFI_HANDLE                    ControllerHandle,
  IN  EFI_HANDLE                    *DriverImageHandle    OPTIONAL,
  IN  EFI_DEVICE_PATH_PROTOCOL      *RemainingDevicePath  OPTIONAL,
  IN  BOOLEAN                       Recursive
  )
{
  EFI_STATUS        Status;

  if (ConnectControllerNesting > 0) {
    return mOriginalConnectController (
      ControllerHandle,
      DriverImageHandle,
      RemainingDevicePath,
      Recursive
    );
  }

  ConnectControllerNesting++;
  DEBUG ((DEBUG_INFO, "WRAP: -> WrappedConnectController h = %p n = %u\n", ControllerHandle, ConnectControllerNesting - 1));
  DebugPrintDevicePathForHandle (DEBUG_INFO, "WRAP: Connecting controller", ControllerHandle);

  Status = mOriginalConnectController (
    ControllerHandle,
    DriverImageHandle,
    RemainingDevicePath,
    Recursive
  );

  DEBUG ((DEBUG_INFO, "WRAP: <- WrappedConnectController h = %p n = %u - %r\n", ControllerHandle, ConnectControllerNesting - 1, Status));
  ConnectControllerNesting--;

  return Status;
}

VOID
WrapConnectController (
  EFI_BOOT_SERVICES *lBS
  )
{
    mOriginalConnectController = lBS->ConnectController;
    lBS->ConnectController = WrappedConnectController;
}

VOID
UnwrapConnectController (
  EFI_BOOT_SERVICES *lBS
  )
{
    lBS->ConnectController = mOriginalConnectController;
}

//STATIC
EFI_STATUS
EFIAPI
WrappedOpenProtocol (
  IN  EFI_HANDLE                Handle,
  IN  EFI_GUID                  *Protocol,
  OUT VOID                      **Interface  OPTIONAL,
  IN  EFI_HANDLE                AgentHandle,
  IN  EFI_HANDLE                ControllerHandle,
  IN  UINT32                    Attributes
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN Nested = FALSE;

  if (BlockEm (Protocol, &Status, Nested)) {
    return Status;
  }

  if (!mAllowLogConnect || ConnectControllerNesting > 0 || Nested) {
    return mOriginalOpenProtocol (
      Handle,
      Protocol,
      Interface,
      AgentHandle,
      ControllerHandle,
      Attributes
    );
  }

  Nested = TRUE;
  DEBUG ((DEBUG_INFO, "WRAP: -> OpenProtocol %g\n", Protocol, Status));

  Status = mOriginalOpenProtocol (
    Handle,
    Protocol,
    Interface,
    AgentHandle,
    ControllerHandle,
    Attributes
  );

  DEBUG ((DEBUG_INFO, "WRAP: <- OpenProtocol %g - %r\n", Protocol, Status));
  Nested = FALSE;

  return Status;
}

//STATIC
EFI_STATUS
EFIAPI
WrappedLocateHandleBuffer (
  IN     EFI_LOCATE_SEARCH_TYPE       SearchType,
  IN     EFI_GUID                     *Protocol       OPTIONAL,
  IN     VOID                         *SearchKey      OPTIONAL,
  OUT    UINTN                        *NoHandles,
  OUT    EFI_HANDLE                   **Buffer
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN Nested = FALSE;

  if (BlockEm (Protocol, &Status, Nested)) {
    return Status;
  }

  if (!mAllowLogConnect || ConnectControllerNesting > 0 || Nested) {
    return mOriginalLocateHandleBuffer (
      SearchType,
      Protocol,
      SearchKey,
      NoHandles,
      Buffer
    );
  }

  Nested = TRUE;

  DEBUG ((DEBUG_INFO, "WRAP: -> LocateHandleBuffer %u %g %p\n",
    SearchType,
    Protocol,
    SearchKey
  ));

  Status = mOriginalLocateHandleBuffer (
    SearchType,
    Protocol,
    SearchKey,
    NoHandles,
    Buffer
  );

  DEBUG ((DEBUG_INFO, "WRAP: <- LocateHandleBuffer %u %g %p %u - %r\n",
    SearchType,
    Protocol,
    SearchKey,
    *NoHandles,
    Status
  ));

  Nested = FALSE;

  return Status;
}

//STATIC
EFI_STATUS
EFIAPI
WrappedLocateProtocol (
  IN  EFI_GUID  *Protocol,
  IN  VOID      *Registration OPTIONAL,
  OUT VOID      **Interface
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN Nested = FALSE;

  if (BlockEm (Protocol, &Status, Nested)) {
    return Status;
  }

  if (!mAllowLogConnect || ConnectControllerNesting > 0 || Nested) {
    return mOriginalLocateProtocol (Protocol, Registration, Interface);
  }

  Nested = TRUE;

  DEBUG ((
    DEBUG_INFO,
    "WRAP: -> LocateProtocol %g %p\n",
    Protocol,
    Registration
  ));

  Status = mOriginalLocateProtocol (Protocol, Registration, Interface);

  DEBUG ((
    DEBUG_INFO,
    "WRAP: <- LocateProtocol %g %p %p - %r\n",
    Protocol,
    Registration,
    *Interface,
    Status
  ));

  Nested = FALSE;

  return Status;
}

VOID
WrapLocateHandleBuffer (
  EFI_BOOT_SERVICES *lBS
  )
{
  // mOriginalOpenProtocol = lBS->OpenProtocol;
  // lBS->OpenProtocol = WrappedOpenProtocol;

  mOriginalLocateHandleBuffer = lBS->LocateHandleBuffer;
  lBS->LocateHandleBuffer = WrappedLocateHandleBuffer;

  // mOriginalLocateProtocol = lBS->LocateProtocol;
  // lBS->LocateProtocol = WrappedLocateProtocol;
}

VOID
UnwrapLocateHandleBuffer (
  EFI_BOOT_SERVICES *lBS
  )
{
  // lBS->LocateProtocol = mOriginalLocateProtocol;
  lBS->LocateHandleBuffer = mOriginalLocateHandleBuffer;
  // lBS->OpenProtocol = mOriginalOpenProtocol;
}

VOID
WrapConnectAll (
  VOID
  )
{
  EFI_STATUS Status;

  DEBUG ((DEBUG_INFO, "WRAP: -> WrapConnectAll\n"));

  Status = OcGetAppleFirmwareUI (&mUIProtocol, NULL);
  DEBUG ((DEBUG_INFO, "WRAP: OcGetAppleFirmwareUI - %r\n", Status));
  if (!EFI_ERROR (Status)) {
    Status = LocateDxeServicesTable ();
    DEBUG ((DEBUG_INFO, "WRAP: LocateDxeServicesTable - %r\n", Status));
  }
  if (EFI_ERROR (Status)) {
    return;
  }

  WrapConnectController (gBS);
  WrapLocateHandleBuffer (gBS);
  WrapDispatch (mDS);
  WrapConnectGop (mUIProtocol);
  WrapDrawBuffer (mUIProtocol);
  WrapSetMode ();

  mAllowLogConnect = TRUE;

  DEBUG ((DEBUG_INFO, "WRAP: <- WrapConnectAll\n"));
}

VOID
UnwrapConnectAll (
  VOID
  )
{
  if (mUIProtocol != NULL) {
    UnwrapSetMode ();
    UnwrapDrawBuffer (mUIProtocol);
    UnwrapConnectGop (mUIProtocol);
    UnwrapDispatch (mDS);
    UnwrapLocateHandleBuffer (gBS);
    UnwrapConnectController (gBS);
  }
}

/**
  Simplified start image routine for OcImageLoaderLoad.

  @param[in]   OcLoadedImage     Our loaded image instance.
  @param[in]   ImageHandle       Handle of image to be started.
  @param[out]  ExitDataSize      The pointer to the size, in bytes, of ExitData.
  @param[out]  ExitData          The pointer to a pointer to a data buffer that includes a Null-terminated
                                 string, optionally followed by additional binary data.

  @retval EFI_SUCCESS on success.
**/
STATIC
EFI_STATUS
InternalDirectStartImage (
  IN  OC_LOADED_IMAGE_PROTOCOL  *OcLoadedImage,
  IN  EFI_HANDLE                ImageHandle,
  OUT UINTN                     *ExitDataSize,
  OUT CHAR16                    **ExitData OPTIONAL
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  LastImage;
  UINTN       SetJumpFlag;

  DEBUG ((DEBUG_INFO, "WRAP: D\n"));

  //
  // Push the current image.
  //
  LastImage           = mCurrentImageHandle;
  mCurrentImageHandle = ImageHandle;

  //
  // Set long jump for Exit() support
  // JumpContext must be aligned on a CPU specific boundary.
  // Overallocate the buffer and force the required alignment
  //
  OcLoadedImage->JumpBuffer = AllocatePool (
                                sizeof (BASE_LIBRARY_JUMP_BUFFER) + BASE_LIBRARY_JUMP_BUFFER_ALIGNMENT
                                );
  if (OcLoadedImage->JumpBuffer == NULL) {
    //
    // Pop the current start image context
    //
    mCurrentImageHandle = LastImage;
    return EFI_OUT_OF_RESOURCES;
  }

  OcLoadedImage->JumpContext = ALIGN_POINTER (
                                 OcLoadedImage->JumpBuffer,
                                 BASE_LIBRARY_JUMP_BUFFER_ALIGNMENT
                                 );

  SetJumpFlag = SetJump (OcLoadedImage->JumpContext);
  //
  // The initial call to SetJump() must always return 0.
  // Subsequent calls to LongJump() cause a non-zero value to be returned by SetJump().
  //
  if (SetJumpFlag == 0) {
    DEBUG ((DEBUG_INFO, "WRAP: E\n"));
    //
    // Invoke the manually loaded image entry point.
    //
    DEBUG ((DEBUG_INFO, "OCB: Starting image %p\n", ImageHandle));
    OcLoadedImage->Started = TRUE;

    WrapConnectAll ();

    OcLoadedImage->Status  = OcLoadedImage->EntryPoint (
                                              ImageHandle,
                                              OcLoadedImage->LoadedImage.SystemTable
                                              );

    UnwrapConnectAll ();

    //
    // If the image returns, exit it through Exit()
    //
    InternalDirectExit (OcLoadedImage, ImageHandle, OcLoadedImage->Status, 0, NULL);
  }

  FreePool (OcLoadedImage->JumpBuffer);

  //
  // Pop the current image.
  //
  mCurrentImageHandle = LastImage;

  //
  // NOTE: EFI 1.10 is not supported, refer to
  // https://github.com/tianocore/edk2/blob/d8dd54f071cfd60a2dcf5426764a89cd91213420/MdeModulePkg/Core/Dxe/Image/Image.c#L1686-L1697
  //

  //
  //  Return the exit data to the caller
  //
  if ((ExitData != NULL) && (ExitDataSize != NULL)) {
    *ExitDataSize = OcLoadedImage->ExitDataSize;
    *ExitData     = OcLoadedImage->ExitData;
  } else if (OcLoadedImage->ExitData != NULL) {
    //
    // Caller doesn't want the exit data, free it
    //
    FreePool (OcLoadedImage->ExitData);
    OcLoadedImage->ExitData = NULL;
  }

  //
  // Save the Status because Image will get destroyed if it is unloaded.
  //
  Status = OcLoadedImage->Status;

  //
  // If the image returned an error, or if the image is an application
  // unload it
  //
  if (  EFI_ERROR (OcLoadedImage->Status)
     || (OcLoadedImage->Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION))
  {
    InternalDirectUnloadImage (OcLoadedImage, ImageHandle);
  }

  return Status;
}

/**
  Detect kernel capabilities from EfiBoot image.

  @param[in] SourceBuffer  Buffer containing EfiBoot.
  @param[in] SourceSize    Size of EfiBoot buffer.

  @returns OC_KERN_CAPABILITY bitmask.
**/
STATIC
UINT32
DetectCapabilities (
  IN  VOID    *SourceBuffer,
  IN  UINT32  SourceSize
  )
{
  BOOLEAN  Exists;
  UINT32   Result;

  //
  // Find Mac OS X version pattern.
  // This pattern started to appear with 10.7.
  //
  Result = 0;
  Exists = FindPattern (
             (CONST UINT8 *)"Mac OS X 10.",
             NULL,
             L_STR_LEN ("Mac OS X 10."),
             SourceBuffer,
             SourceSize - sizeof (UINT32),
             &Result
             );

 #ifdef MDE_CPU_IA32
  //
  // For IA32 mode the only question is whether we support K32_64.
  // This starts with 10.7, and in theory is valid for some early
  // developer preview 10.8 images, so simply decide on Mac OS X
  // version pattern presence.
  //
  if (Exists) {
    return OC_KERN_CAPABILITY_K32_U64;
  }

  return OC_KERN_CAPABILITY_K32_U32 | OC_KERN_CAPABILITY_K32_U64;
 #else
  //
  // For X64 mode, when the pattern is found, this can be 10.7 or 10.8+.
  // 10.7 supports K32_64 and K64, while newer versions have only K64.
  //
  if (Exists) {
    if (((UINT8 *)SourceBuffer)[Result + L_STR_LEN ("Mac OS X 10.")] == '7') {
      return OC_KERN_CAPABILITY_K32_U64 | OC_KERN_CAPABILITY_K64_U64;
    }

    return OC_KERN_CAPABILITY_K64_U64;
  }

  //
  // The pattern is not found. This can be 10.6 or 10.4~10.5.
  // 10.6 supports K32 and K64, while older versions have only K32.
  // Detect 10.6 by x86_64 pattern presence.
  //
  Result = SourceSize / 2;
  Exists = FindPattern (
             (CONST UINT8 *)"x86_64",
             NULL,
             L_STR_SIZE ("x86_64"),
             SourceBuffer,
             SourceSize - sizeof (UINT32),
             &Result
             );
  if (Exists) {
    return OC_KERN_CAPABILITY_K32_U32 | OC_KERN_CAPABILITY_K32_U64 | OC_KERN_CAPABILITY_K64_U64;
  }

  return OC_KERN_CAPABILITY_K32_U32 | OC_KERN_CAPABILITY_K32_U64;
 #endif
}

STATIC
EFI_STATUS
EFIAPI
InternalEfiLoadImage (
  IN  BOOLEAN                   BootPolicy,
  IN  EFI_HANDLE                ParentImageHandle,
  IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN  VOID                      *SourceBuffer OPTIONAL,
  IN  UINTN                     SourceSize,
  OUT EFI_HANDLE                *ImageHandle
  )
{
  EFI_STATUS  SecureBootStatus;
  EFI_STATUS  Status;
  VOID        *AllocatedBuffer;
  UINT32      RealSize;

  if ((ParentImageHandle == NULL) || (ImageHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((SourceBuffer == NULL) && (DevicePath == NULL)) {
    return EFI_NOT_FOUND;
  }

  if ((SourceBuffer != NULL) && (SourceSize == 0)) {
    return EFI_UNSUPPORTED;
  }

  AllocatedBuffer = NULL;
  if (SourceBuffer == NULL) {
    Status = InternalEfiLoadImageFile (
               DevicePath,
               &SourceSize,
               &SourceBuffer
               );
    if (EFI_ERROR (Status)) {
      Status = InternalEfiLoadImageProtocol (
                 DevicePath,
                 BootPolicy == FALSE,
                 &SourceSize,
                 &SourceBuffer
                 );
    }

    if (!EFI_ERROR (Status)) {
      AllocatedBuffer = SourceBuffer;
    }
  }

  if ((DevicePath != NULL) && (SourceBuffer != NULL) && mImageLoaderEnabled) {
    SecureBootStatus = OcAppleSecureBootVerify (
                         DevicePath,
                         SourceBuffer,
                         SourceSize
                         );
  } else {
    SecureBootStatus = EFI_UNSUPPORTED;
  }

  //
  // A security violation means we should just die.
  //
  if (SecureBootStatus == EFI_SECURITY_VIOLATION) {
    DEBUG ((
      DEBUG_WARN,
      "OCB: Apple Secure Boot prohibits this boot entry, enforcing!\n"
      ));
    return EFI_SECURITY_VIOLATION;
  }

  //
  // By default assume target default.
  //
 #ifdef MDE_CPU_IA32
  mImageLoaderCaps = OC_KERN_CAPABILITY_K32_U32 | OC_KERN_CAPABILITY_K32_U64;
 #else
  mImageLoaderCaps = OC_KERN_CAPABILITY_K64_U64;
 #endif

  if (SourceBuffer != NULL) {
    RealSize = (UINT32)SourceSize;
 #ifdef MDE_CPU_IA32
    Status = FatFilterArchitecture32 ((UINT8 **)&SourceBuffer, &RealSize);
 #else
    Status = FatFilterArchitecture64 ((UINT8 **)&SourceBuffer, &RealSize);
 #endif

    //
    // This is FAT image.
    // Determine its capabilities.
    //
    if (!EFI_ERROR (Status) && (RealSize != SourceSize) && (RealSize >= EFI_PAGE_SIZE)) {
      mImageLoaderCaps = DetectCapabilities (SourceBuffer, RealSize);
    }

    DEBUG ((
      DEBUG_INFO,
      "OCB: Arch filtering %p(%u)->%p(%u) caps %u - %r\n",
      AllocatedBuffer,
      (UINT32)SourceSize,
      SourceBuffer,
      RealSize,
      mImageLoaderCaps,
      Status
      ));

    if (!EFI_ERROR (Status)) {
      SourceSize = RealSize;
    } else if (AllocatedBuffer != NULL) {
      SourceBuffer = NULL;
      SourceSize   = 0;
    }
  }

  if ((SourceBuffer != NULL) && (mImageLoaderPatch != NULL)) {
    mImageLoaderPatch (
      DevicePath,
      SourceBuffer,
      SourceSize
      );
  }

  //
  // Load the image ourselves in secure boot mode.
  //
  if (SecureBootStatus == EFI_SUCCESS) {
    if (SourceBuffer != NULL) {
      Status = OcImageLoaderLoad (
                 FALSE,
                 ParentImageHandle,
                 DevicePath,
                 SourceBuffer,
                 SourceSize,
                 ImageHandle
                 );
    } else {
      //
      // We verified the image, but contained garbage.
      // This should not happen, just abort.
      //
      Status = EFI_UNSUPPORTED;
    }
  } else {
    PreserveGrubShimHooks ();
    Status = mOriginalEfiLoadImage (
               BootPolicy,
               ParentImageHandle,
               DevicePath,
               SourceBuffer,
               SourceSize,
               ImageHandle
               );
    RestoreGrubShimHooks ("LoadImage");
  }

  if (AllocatedBuffer != NULL) {
    FreePool (AllocatedBuffer);
  }

  //
  // Some types of firmware may not update loaded image protocol fields correctly
  // when loading via source buffer. Do it here.
  //
  if (!EFI_ERROR (Status) && (SourceBuffer != NULL) && (DevicePath != NULL)) {
    InternalUpdateLoadedImage (*ImageHandle, DevicePath);
  }

  return Status;
}

BOOLEAN gExternProtocolWrap = FALSE;

STATIC
EFI_STATUS
EFIAPI
InternalEfiStartImage (
  IN  EFI_HANDLE  ImageHandle,
  OUT UINTN       *ExitDataSize,
  OUT CHAR16      **ExitData OPTIONAL
  )
{
  EFI_STATUS                 Status;
  OC_LOADED_IMAGE_PROTOCOL   *OcLoadedImage;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  // EFI_TPL                    CurrentTpl;
  // STATIC UINTN               Nesting = 0;

  DEBUG ((DEBUG_INFO, "WRAP: A\n"));

  //
  // If we loaded the image, invoke the entry point manually.
  //
  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &mOcLoadedImageProtocolGuid,
                  (VOID **)&OcLoadedImage
                  );
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "WRAP: B\n"));
    //
    // Call configure update for our images.
    //
    if (mImageLoaderConfigure != NULL) {
      mImageLoaderConfigure (
        &OcLoadedImage->LoadedImage,
        mImageLoaderCaps
        );
    }

    return InternalDirectStartImage (
             OcLoadedImage,
             ImageHandle,
             ExitDataSize,
             ExitData
             );
  }

  DEBUG ((DEBUG_INFO, "WRAP: C\n"));

  //
  // Call configure update for generic images too.
  //
  if (mImageLoaderConfigure != NULL) {
    Status = gBS->HandleProtocol (
                    ImageHandle,
                    &gEfiLoadedImageProtocolGuid,
                    (VOID **)&LoadedImage
                    );
    if (!EFI_ERROR (Status)) {
      mImageLoaderConfigure (
        LoadedImage,
        mImageLoaderCaps
        );
    }
  }

  PreserveGrubShimHooks ();

  // CurrentTpl = gBS->RaiseTPL (TPL_NOTIFY);
  // DEBUG ((DEBUG_INFO, "OCB: [nesting %u] Lowering TPL %u->0\n", Nesting, CurrentTpl));
  // Nesting++;
  // gBS->RestoreTPL (0);

  if (gExternProtocolWrap) {
#if defined(OC_TARGET_NOOPT)
    WaitForKeyPress (L"Mike...");
#endif
    WrapConnectAll ();
  }
  DEBUG ((DEBUG_INFO, "OCB: >>>\n"));
  Status = mOriginalEfiStartImage (ImageHandle, ExitDataSize, ExitData);
  DEBUG ((DEBUG_INFO, "OCB: <<<\n"));
  if (gExternProtocolWrap) {
    UnwrapConnectAll ();
  }

  // Nesting--;
  // DEBUG ((DEBUG_INFO, "OCB: [nesting %u] Restoring TPL ->%u\n", Nesting, CurrentTpl));
  // gBS->RaiseTPL (CurrentTpl);

  RestoreGrubShimHooks ("StartImage");

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
InternalEfiUnloadImage (
  IN  EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                Status;
  OC_LOADED_IMAGE_PROTOCOL  *OcLoadedImage;

  //
  // If we loaded the image, do the unloading manually.
  //
  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &mOcLoadedImageProtocolGuid,
                  (VOID **)&OcLoadedImage
                  );
  if (!EFI_ERROR (Status)) {
    return InternalDirectUnloadImage (
             OcLoadedImage,
             ImageHandle
             );
  }

  return mOriginalEfiUnloadImage (ImageHandle);
}

STATIC
EFI_STATUS
EFIAPI
InternalEfiExit (
  IN  EFI_HANDLE  ImageHandle,
  IN  EFI_STATUS  ExitStatus,
  IN  UINTN       ExitDataSize,
  IN  CHAR16      *ExitData     OPTIONAL
  )
{
  EFI_STATUS                Status;
  OC_LOADED_IMAGE_PROTOCOL  *OcLoadedImage;

  //
  // If we loaded the image, do the exit manually.
  //
  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &mOcLoadedImageProtocolGuid,
                  (VOID **)&OcLoadedImage
                  );

  DEBUG ((DEBUG_VERBOSE, "OCB: InternalEfiExit %p - %r / %r\n", ImageHandle, ExitStatus, Status));

  if (!EFI_ERROR (Status)) {
    return InternalDirectExit (
             OcLoadedImage,
             ImageHandle,
             ExitStatus,
             ExitDataSize,
             ExitData
             );
  }

  PreserveGrubShimHooks ();
  Status = mOriginalEfiExit (ImageHandle, ExitStatus, ExitDataSize, ExitData);
  RestoreGrubShimHooks ("Exit");

  return Status;
}

VOID
OcImageLoaderInit (
  IN     CONST BOOLEAN  ProtectUefiServices
  )
{
  mProtectUefiServices = ProtectUefiServices;

  mOriginalEfiLoadImage   = gBS->LoadImage;
  mOriginalEfiStartImage  = gBS->StartImage;
  mOriginalEfiUnloadImage = gBS->UnloadImage;
  mOriginalEfiExit        = gBS->Exit;

  gBS->LoadImage   = InternalEfiLoadImage;
  gBS->StartImage  = InternalEfiStartImage;
  gBS->UnloadImage = InternalEfiUnloadImage;
  gBS->Exit        = InternalEfiExit;

  gBS->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
}

VOID
OcImageLoaderActivate (
  VOID
  )
{
  mImageLoaderEnabled = TRUE;
}

VOID
OcImageLoaderRegisterPatch (
  IN OC_IMAGE_LOADER_PATCH  Patch      OPTIONAL
  )
{
  mImageLoaderPatch = Patch;
}

VOID
OcImageLoaderRegisterConfigure (
  IN OC_IMAGE_LOADER_CONFIGURE  Configure  OPTIONAL
  )
{
  mImageLoaderConfigure = Configure;
}
