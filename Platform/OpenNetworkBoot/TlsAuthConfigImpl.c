/** @file
  Miscellaneous routines for TLS auth config.

  Copyright (c) 2016 - 2018, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2024, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "NetworkBootInternal.h"

#define TLS_AUTH_CONFIG_VAR_BASE_ATTR  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)

typedef
EFI_STATUS
(*PROCESS_CERT)(
  IN VOID                 *Context,
  IN UINTN                CertIndex,
  IN UINTN                CertSize,
  IN EFI_SIGNATURE_DATA   *Cert
  );

typedef struct {
  EFI_GUID    *OwnerGuid;
  UINTN       X509DataSize;
  VOID        *X509Data;
} CERT_IS_PRESENT_CONTEXT;

/**
  Perform function for all signatures in specified database.

  @param[in]    VariableName        The variable name of the vendor's signature database.
  @param[in]    VendorGuid          A unique identifier for the signature database vendor.
  @param[in]    ProcessCert         The method to call for each certificate.
  @param[in]    Context             Context for ProcessCert, if required.

  @retval   EFI_SUCCESS            Looped over all signatures.
  @retval   EFI_OUT_OF_RESOURCES   Could not allocate needed resources.
  @retval   Other                  Other error or return code from from ProcessCert.
**/
STATIC
EFI_STATUS
LoopOverInstalledCerts (
  IN CHAR16                        *VariableName,
  IN EFI_GUID                      *VendorGuid,
  IN PROCESS_CERT                  ProcessCert,
  IN VOID                          *Context OPTIONAL
  )
{
  EFI_STATUS          Status;
  UINT32              Index;
  UINTN               CertCount;
  UINTN               GuidIndex;
  UINTN               DataSize;
  UINT8               *Data;
  EFI_SIGNATURE_LIST  *CertList;
  EFI_SIGNATURE_DATA  *Cert;
  UINT32              ItemDataSize;

  ASSERT (ProcessCert != NULL);

  Data              = NULL;
  CertList          = NULL;
  Cert              = NULL;

  //
  // Read Variable.
  //
  DataSize = 0;
  Status   = gRT->GetVariable (VariableName, VendorGuid, NULL, &DataSize, Data);
  if (EFI_ERROR (Status) && (Status != EFI_BUFFER_TOO_SMALL)) {
    if (Status == EFI_NOT_FOUND) {
      Status = EFI_SUCCESS;
    }
    return Status;
  }

  Data = (UINT8 *)AllocateZeroPool (DataSize);
  if (Data == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gRT->GetVariable (VariableName, VendorGuid, NULL, &DataSize, Data);
  if (EFI_ERROR (Status)) {
    FreePool (Data);
    return Status;
  }

  //
  // Enumerate all data.
  //
  ItemDataSize = (UINT32)DataSize;
  CertList     = (EFI_SIGNATURE_LIST *)Data;
  GuidIndex    = 0;

  while ((ItemDataSize > 0) && (ItemDataSize >= CertList->SignatureListSize)) {
    if (!CompareGuid (&CertList->SignatureType, &gEfiCertX509Guid)) {
      //
      // The signature type is not supported in current implementation.
      //
      ItemDataSize -= CertList->SignatureListSize;
      CertList      = (EFI_SIGNATURE_LIST *)((UINT8 *)CertList + CertList->SignatureListSize);
      continue;
    }

    CertCount = (CertList->SignatureListSize - sizeof (EFI_SIGNATURE_LIST) - CertList->SignatureHeaderSize) / CertList->SignatureSize;
    for (Index = 0; Index < CertCount; Index++) {
      Cert = (EFI_SIGNATURE_DATA *)((UINT8 *)CertList
                                    + sizeof (EFI_SIGNATURE_LIST)
                                    + CertList->SignatureHeaderSize
                                    + Index * CertList->SignatureSize);

      Status = ProcessCert (Context, GuidIndex, CertList->SignatureListSize, Cert);
      if (EFI_ERROR (Status)) {
        break;
      }

      ++GuidIndex;
    }

    ItemDataSize -= CertList->SignatureListSize;
    CertList      = (EFI_SIGNATURE_LIST *)((UINT8 *)CertList + CertList->SignatureListSize);
  }

  FreePool (Data);

  return Status;
}

/**
  @retval   EFI_SUCCESS             Continue processing.
**/
STATIC
EFI_STATUS
LogCert (
  IN VOID                 *Context,
  IN UINTN                CertIndex,
  IN UINTN                CertSize,
  IN EFI_SIGNATURE_DATA   *Cert
  )
{
  DEBUG ((DEBUG_INFO, "NTBT: Cert %g\n", &Cert->SignatureOwner));
  return EFI_SUCCESS;
}

/**
  Log owner GUID of each installed certificate in signature database.

  @param[in]    VariableName        The variable name of the signature database.
  @param[in]    VendorGuid          A unique identifier for the signature database vendor.

  @retval   EFI_SUCCESS             Success.
**/
EFI_STATUS
LogInstalledCerts (
  IN CHAR16                        *VariableName,
  IN EFI_GUID                      *VendorGuid
  )
{
  return LoopOverInstalledCerts (
    VariableName,
    VendorGuid,
    LogCert,
    NULL
  );
}

/**
  @retval   EFI_SUCCESS             Success, certificate not found; continue processing.
  @retval   EFI_ALREADY_STARTED     Success, certificate found; stop processing for more.
**/
STATIC
EFI_STATUS
CheckCertPresent (
  IN VOID                 *VoidContext,
  IN UINTN                CertIndex,
  IN UINTN                CertSize,
  IN EFI_SIGNATURE_DATA   *Cert
  )
{
  CERT_IS_PRESENT_CONTEXT     *Context;

  Context = VoidContext;

  if (!CompareGuid (&Cert->SignatureOwner, Context->OwnerGuid)) {
    return EFI_SUCCESS;
  }

  if ( (CertSize == Context->X509DataSize)
    && (CompareMem (Cert->SignatureData, Context->X509Data, CertSize) == 0)
  ) {
    return EFI_ALREADY_STARTED;
  }

  return EFI_SUCCESS;
}

/**
  Report whether specified signature is already enrolled for given owner.

  @param[in] VariableName    Variable name of CA database.
  @param[in] VendorGuid      Unique identifier for the CA database vendor.
  @param[in] OwnerGuid       Unique identifier for owner of the certificate to be searched for.
  @param[in] X509DataSize    Certificate data size.
  @param[in] X509Data        Certificate data.

  @retval   EFI_SUCCESS            Certificate is already enrolled.
  @retval   EFI_OUT_OF_RESOURCES   Could not allocate needed resources.
**/
EFI_STATUS
CertIsPresent (
  IN CHAR16                        *VariableName,
  IN EFI_GUID                      *VendorGuid,
  IN EFI_GUID                      *OwnerGuid,
  IN UINTN                         X509DataSize,
  IN VOID                          *X509Data
  )
{
  CERT_IS_PRESENT_CONTEXT     Context;

  ASSERT (X509Data != NULL);

  Context.OwnerGuid      = OwnerGuid;
  Context.X509DataSize  = X509DataSize;
  Context.X509Data      = X509Data;

  return LoopOverInstalledCerts (
    VariableName,
    VendorGuid,
    CheckCertPresent,
    &Context
  );
}

/**
  Delete all entries with specified owner from signature database.
  (Modified from EDK 2 DeleteCertsForOwner which removes one cert, identified by index.)

  @param[in]    VariableName        The variable name of the signature database.
  @param[in]    VendorGuid          A unique identifier for the signature database vendor.
  @param[in]    OwnerGuid           A unique identifier for owner of the certificate to be installed.

  @retval   EFI_SUCCESS             Delete signature successfully.
  @retval   EFI_OUT_OF_RESOURCES    Could not allocate needed resources.
**/
EFI_STATUS
DeleteCertsForOwner (
  IN CHAR16                        *VariableName,
  IN EFI_GUID                      *VendorGuid,
  IN EFI_GUID                      *OwnerGuid
  )
{
  EFI_STATUS          Status;
  UINTN               DataSize;
  UINT8               *Data;
  UINT8               *OldData;
  UINT32              Attr;
  UINT32              Index;
  EFI_SIGNATURE_LIST  *CertList;
  EFI_SIGNATURE_LIST  *NewCertList;
  EFI_SIGNATURE_DATA  *Cert;
  UINTN               CertCount;
  UINT32              Offset;
  BOOLEAN             IsItemFound;
  UINT32              ItemDataSize;

  Data     = NULL;
  OldData  = NULL;
  CertList = NULL;
  Cert     = NULL;
  Attr     = 0;

  //
  // Get original signature list data.
  //
  DataSize = 0;
  Status   = gRT->GetVariable (VariableName, VendorGuid, NULL, &DataSize, NULL);
  if (EFI_ERROR (Status) && (Status != EFI_BUFFER_TOO_SMALL)) {
    if (Status == EFI_NOT_FOUND) {
      Status = EFI_SUCCESS;
    }
    return Status;
  }

  OldData = AllocateZeroPool (DataSize);
  if (OldData == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gRT->GetVariable (VariableName, VendorGuid, &Attr, &DataSize, OldData);
  if (EFI_ERROR (Status)) {
    FreePool (OldData);
    return Status;
  }

  //
  // Allocate space for new variable.
  //
  Data = AllocateZeroPool (DataSize);
  if (Data == NULL) {
    FreePool (OldData);
    return  EFI_OUT_OF_RESOURCES;
  }

  //
  // Enumerate all data, erasing target items.
  //
  IsItemFound  = FALSE;
  ItemDataSize = (UINT32)DataSize;
  CertList     = (EFI_SIGNATURE_LIST *)OldData;
  Offset       = 0;
  while ((ItemDataSize > 0) && (ItemDataSize >= CertList->SignatureListSize)) {
    if (CompareGuid (&CertList->SignatureType, &gEfiCertX509Guid)) {
      //
      // Copy EFI_SIGNATURE_LIST header then calculate the signature count in this list.
      //
      CopyMem (Data + Offset, CertList, (sizeof (EFI_SIGNATURE_LIST) + CertList->SignatureHeaderSize));
      NewCertList = (EFI_SIGNATURE_LIST *)(Data + Offset);
      Offset     += (sizeof (EFI_SIGNATURE_LIST) + CertList->SignatureHeaderSize);
      Cert        = (EFI_SIGNATURE_DATA *)((UINT8 *)CertList + sizeof (EFI_SIGNATURE_LIST) + CertList->SignatureHeaderSize);
      CertCount   = (CertList->SignatureListSize - sizeof (EFI_SIGNATURE_LIST) - CertList->SignatureHeaderSize) / CertList->SignatureSize;
      for (Index = 0; Index < CertCount; Index++) {
        if (CompareGuid (&Cert->SignatureOwner, OwnerGuid)) {
          //
          // Find it! Skip it!
          //
          NewCertList->SignatureListSize -= CertList->SignatureSize;
          IsItemFound                     = TRUE;
        } else {
          //
          // This item doesn't match. Copy it to the Data buffer.
          //
          CopyMem (Data + Offset, (UINT8 *)(Cert), CertList->SignatureSize);
          Offset += CertList->SignatureSize;
        }

        Cert = (EFI_SIGNATURE_DATA *)((UINT8 *)Cert + CertList->SignatureSize);
      }
    } else {
      //
      // This List doesn't match. Just copy it to the Data buffer.
      //
      CopyMem (Data + Offset, (UINT8 *)(CertList), CertList->SignatureListSize);
      Offset += CertList->SignatureListSize;
    }

    ItemDataSize -= CertList->SignatureListSize;
    CertList      = (EFI_SIGNATURE_LIST *)((UINT8 *)CertList + CertList->SignatureListSize);
  }

  if (IsItemFound) {
    //
    // Delete the EFI_SIGNATURE_LIST header if there is no signature remaining in any list.
    //
    ItemDataSize = Offset;
    CertList     = (EFI_SIGNATURE_LIST *)Data;
    Offset       = 0;
    ZeroMem (OldData, ItemDataSize);
    while ((ItemDataSize > 0) && (ItemDataSize >= CertList->SignatureListSize)) {
      CertCount = (CertList->SignatureListSize - sizeof (EFI_SIGNATURE_LIST) - CertList->SignatureHeaderSize) / CertList->SignatureSize;
      if (CertCount != 0) {
        CopyMem (OldData + Offset, (UINT8 *)(CertList), CertList->SignatureListSize);
        Offset += CertList->SignatureListSize;
      }

      ItemDataSize -= CertList->SignatureListSize;
      CertList      = (EFI_SIGNATURE_LIST *)((UINT8 *)CertList + CertList->SignatureListSize);
    }

    DataSize = Offset;

    //
    // Set (or delete if everything was removed) the Variable.
    //
    Status = gRT->SetVariable (
                    VariableName,
                    VendorGuid,
                    Attr,
                    DataSize,
                    OldData
                    );
  }

  FreePool (Data);
  FreePool (OldData);

  return Status;
}

/**
  Enroll a new X509 certificate into Variable.

  @param[in] VariableName    Variable name of CA database.
  @param[in] VendorGuid      Unique identifier for the CA database vendor.
  @param[in] OwnerGuid       Unique identifier for owner of the certificate to be installed.
  @param[in] X509DataSize    Certificate data size.
  @param[in] X509Data        Certificate data.

  @retval   EFI_SUCCESS            New X509 is enrolled successfully.
  @retval   EFI_OUT_OF_RESOURCES   Could not allocate needed resources.
**/
EFI_STATUS
EnrollX509toVariable (
  IN CHAR16                        *VariableName,
  IN EFI_GUID                      *VendorGuid,
  IN EFI_GUID                      *OwnerGuid,
  IN UINTN                         X509DataSize,
  IN VOID                          *X509Data
  )
{
  EFI_STATUS          Status;
  EFI_SIGNATURE_LIST  *CACert;
  EFI_SIGNATURE_DATA  *CACertData;
  VOID                *Data;
  UINTN               DataSize;
  UINTN               SigDataSize;
  UINT32              Attr;

  SigDataSize  = 0;
  DataSize     = 0;
  CACert       = NULL;
  CACertData   = NULL;
  Data         = NULL;
  Attr         = 0;

  ASSERT (X509Data != NULL);

  SigDataSize = sizeof (EFI_SIGNATURE_LIST) + sizeof (EFI_SIGNATURE_DATA) - 1 + X509DataSize;

  Data = AllocateZeroPool (SigDataSize);
  if (Data == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Fill Certificate Database parameters.
  //
  CACert                      = (EFI_SIGNATURE_LIST *)Data;
  CACert->SignatureListSize   = (UINT32)SigDataSize;
  CACert->SignatureHeaderSize = 0;
  CACert->SignatureSize       = (UINT32)(sizeof (EFI_SIGNATURE_DATA) - 1 + X509DataSize);
  CopyGuid (&CACert->SignatureType, &gEfiCertX509Guid);

  CACertData = (EFI_SIGNATURE_DATA *)((UINT8 *)CACert + sizeof (EFI_SIGNATURE_LIST));
  CopyGuid (&CACertData->SignatureOwner, OwnerGuid);
  CopyMem ((UINT8 *)(CACertData->SignatureData), X509Data, X509DataSize);

  //
  // Check if the signature database entry already exists. If it does, use the
  // EFI_VARIABLE_APPEND_WRITE attribute to append the new signature data to
  // the original variable, plus preserve the original variable attributes.
  //
  Status = gRT->GetVariable (
                  VariableName,
                  VendorGuid,
                  &Attr,
                  &DataSize,
                  NULL
                  );
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Attr |= EFI_VARIABLE_APPEND_WRITE;
  } else if (Status == EFI_NOT_FOUND) {
    Attr = TLS_AUTH_CONFIG_VAR_BASE_ATTR;
  } else {
    FreePool (Data);
    return Status;
  }

  Status = gRT->SetVariable (
                  VariableName,
                  VendorGuid,
                  Attr,
                  SigDataSize,
                  Data
                  );

  FreePool (Data);

  return Status;
}
