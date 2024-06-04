/** @file
  Callback handler for HTTP Boot.

  Copyright (c) 2024, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "NetworkBootInternal.h"

STATIC EFI_HTTP_BOOT_CALLBACK mOriginalHttpBootCallback;

STATIC
EFI_STATUS
EFIAPI
OpenNetworkBootHttpBootCallback (
  IN EFI_HTTP_BOOT_CALLBACK_PROTOCOL    *This,
  IN EFI_HTTP_BOOT_CALLBACK_DATA_TYPE   DataType,
  IN BOOLEAN                            Received,
  IN UINT32                             DataLength,
  IN VOID                               *Data   OPTIONAL
  )
{
  return mOriginalHttpBootCallback (
      This,
      DataType,
      Received,
      DataLength,
      Data
  );
}

STATIC
VOID
EFIAPI
NotifyInstallHttpBootCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                        Status;
  EFI_HANDLE                        LoadFileHandle;
  EFI_HTTP_BOOT_CALLBACK_PROTOCOL   *HttpBootCallback;

  LoadFileHandle = Context;

  Status = gBS->HandleProtocol (
                LoadFileHandle,
                &gEfiHttpBootCallbackProtocolGuid,
                (VOID **) &HttpBootCallback
                );

  if (!EFI_ERROR (Status)) {
    //
    // Our hook will stay installed until they do - or don't* - uninstall
    // their callback. But it won't get called after our calls to LoadFile
    // have finished.
    // *REF: https://edk2.groups.io/g/devel/message/117469
    // TODO: Add edk2 bugzilla issue.
    //
    mOriginalHttpBootCallback = HttpBootCallback->Callback;
    HttpBootCallback->Callback = OpenNetworkBootHttpBootCallback;
  }
}

EFI_EVENT
MonitorHttpBootCallback (
  EFI_HANDLE    LoadFileHandle
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;
  VOID        *Registration;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  NotifyInstallHttpBootCallback,
                  LoadFileHandle,
                  &Event
                  );

  if (EFI_ERROR (Status)) {
    return NULL;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gEfiHttpBootCallbackProtocolGuid,
                  Event,
                  &Registration
                  );

  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (Event);
    return NULL;
  }

  return Event;
}
