/** @file
  Callback handler for HTTP Boot.

  Copyright (c) 2024, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "NetworkBootInternal.h"

#include <Protocol/Http.h>

OC_DMG_LOADING_SUPPORT gDmgLoading;

STATIC EFI_HTTP_BOOT_CALLBACK mOriginalHttpBootCallback;

//
// Abort if we are loading a .dmg and these are banned, or if underlying drivers have
// allowed http:// in URL but user setting for OpenNetworkBoot does not allow it.
// If PcdAllowHttpConnections was not set (via NETWORK_ALLOW_HTTP_CONNECTIONS compilation
// flag) then both HttpDxe and HttpBootDxe will enforce https:// before we get to here.
//
EFI_STATUS
ValidateDmgAndHttps (
  CHAR16                    *Uri,
  BOOLEAN                   ShowLog
  )
{
  CHAR8                     *Match;
  CHAR8                     *Uri8;
  UINTN                     UriSize;
  BOOLEAN                   HasDmgExtension;

  
  if (gRequireHttpsUri && !HasHttpsUri (Uri)) {
    //
    // Do not return ACCESS_DENIED as this will attempt to add authentication to the request.
    //
    if (ShowLog) {
      DEBUG ((DEBUG_INFO, "NTBT: Invalid URI https:// is required\n"));
    }
    return EFI_UNSUPPORTED;
  }

  if (gDmgLoading == OcDmgLoadingDisabled) {
    UriSize = StrSize (Uri);
    Uri8 = AllocatePool (UriSize);
    if (Uri8 == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    UnicodeStrToAsciiStrS (Uri, Uri8, UriSize);

    Match = ".dmg";
    HasDmgExtension = UriFileHasExtension (Uri8, Match);
    if (!HasDmgExtension) {
      Match = ".chunklist";
      HasDmgExtension = UriFileHasExtension (Uri8, Match);
    }
    FreePool (Uri8);
    if (HasDmgExtension)
    {
      if (ShowLog) {
        DEBUG ((DEBUG_INFO, "NTBT: %a file is requested while DMG loading is disabled\n", Match));
      }
      return EFI_UNSUPPORTED;
    }
  }

  return EFI_SUCCESS;
}

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
  EFI_STATUS          Status;
  EFI_HTTP_MESSAGE    *HttpMessage;

  if ((DataType == HttpBootHttpRequest) && (Data != NULL)) {
    HttpMessage = (EFI_HTTP_MESSAGE *)Data;
    if (HttpMessage->Data.Request->Url != NULL) {
      //
      // Print log messages once on initial access with HTTP HEAD, don't
      // log on subsequent GET which is an attempt to get file size by
      // pre-loading entire file for case of chunked encoding (where file
      // size is not known until it has been transferred).
      //
      Status = ValidateDmgAndHttps (
        HttpMessage->Data.Request->Url,
        HttpMessage->Data.Request->Method == HttpMethodHead
      );
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

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
