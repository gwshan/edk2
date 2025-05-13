/** @file
  Library that implements the Arm CCA Realm Service Interface calls.

  Copyright (c) 2022 - 2023, Arm Limited. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
    - Rsi or RSI   - Realm Service Interface
    - IPA          - Intermediate Physical Address
    - RIPAS        - Realm IPA state

  @par Reference(s):
   - Realm Management Monitor (RMM) Specification, version A-bet0
     (https://developer.arm.com/documentation/den0137/)

**/
#include <Base.h>

#include <Library/ArmCcaRsiLib.h>
#include <Library/ArmLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include "ArmCcaRsi.h"

/**
  Convert the RSI status code to EFI Status code.

  @param [in]   RsiCommandReturnCode  RSI status code.

  @retval RETURN_SUCCESS            Success.
  @retval RETURN_INVALID_PARAMETER  A parameter is invalid.
  @retval RETURN_ABORTED            The operation was aborted as the state
                                    of the Realm or REC does not match the
                                    state expected by the command.
  @retval RETURN_NOT_READY          The operation requested by the command
                                    is not complete.
 **/
STATIC
RETURN_STATUS
RsiCmdStatusToEfiStatus (
  IN  UINT64  RsiCommandReturnCode
  )
{
  switch (RsiCommandReturnCode) {
    case RSI_SUCCESS:
      return RETURN_SUCCESS;
    case RSI_ERROR_INPUT:
      return RETURN_INVALID_PARAMETER;
    case RSI_ERROR_STATE:
      return RETURN_ABORTED;
    case RSI_INCOMPLETE:
      return RETURN_NOT_READY;
    default:
      // Unknown error code.
      ASSERT (0);
      break;
  } // switch

  return RETURN_ABORTED;
}

/**
  Check if the value is aligned to the size of the Realm granule.

  @param [in] Value          Value to check granule alignment.

  @retval TRUE  Value is aligned to the Realm granule size.
  @retval FALSE Value is not aligned to the Realm granule size.
**/
STATIC
BOOLEAN
EFIAPI
IsGranuleAligned (
  IN   UINT64  *Value
  )
{
  if (((UINT64)Value & (REALM_GRANULE_SIZE - 1)) != 0) {
    return FALSE;
  }

  return TRUE;
}

/**
  Returns the IPA state for the page pointed by the address.

  @param [in]   Address     Address to retrive IPA state.
  @param [out]  State       The RIPAS state for the address specified.

  @retval RETURN_SUCCESS            Success.
  @retval RETURN_INVALID_PARAMETER  A parameter is invalid.
**/
RETURN_STATUS
EFIAPI
RsiGetIpaState (
  IN   UINT64  *Address,
  OUT  RIPAS   *State
  )
{
  RETURN_STATUS  Status;
  ARM_SMC_ARGS   SmcCmd;

  if ((State == NULL) || (!IsGranuleAligned (Address))) {
    return RETURN_INVALID_PARAMETER;
  }

  ZeroMem (&SmcCmd, sizeof (SmcCmd));
  SmcCmd.Arg0 = FID_RSI_IPA_STATE_GET;
  SmcCmd.Arg1 = (UINTN)Address;

  ArmCallSmc (&SmcCmd);
  Status = RsiCmdStatusToEfiStatus (SmcCmd.Arg0);
  if (!RETURN_ERROR (Status)) {
    *State = (RIPAS)(SmcCmd.Arg1 & RIPAS_TYPE_MASK);
  }

  return Status;
}

/**
  Sets the IPA state for the pages pointed by the memory range.

  @param [in]   Address     Address to the start of the memory range.
  @param [in]   Size        Length of the memory range.
  @param [in]   State       The RIPAS state to be configured.

  @retval RETURN_SUCCESS            Success.
  @retval RETURN_INVALID_PARAMETER  A parameter is invalid.
**/
RETURN_STATUS
EFIAPI
RsiSetIpaState (
  IN  UINT64  *Address,
  IN  UINT64  Size,
  IN  RIPAS   State
  )
{
  RETURN_STATUS  Status;
  UINT64         *BaseAddress;
  UINT64         *EndAddress;
  ARM_SMC_ARGS   SmcCmd;

  if ((Size == 0) ||
      (!IsGranuleAligned ((UINT64 *)Size)) ||
      (!IsGranuleAligned (Address)))
  {
    return RETURN_INVALID_PARAMETER;
  }

  BaseAddress = Address;
  // Divide Size by 8 for the pointer arithmetic
  // to work, as we are adding to UINT64*.
  EndAddress = Address + (Size >> 3);

  while (Size > 0) {
    ZeroMem (&SmcCmd, sizeof (SmcCmd));
    SmcCmd.Arg0 = FID_RSI_IPA_STATE_SET;
    SmcCmd.Arg1 = (UINTN)BaseAddress;
    SmcCmd.Arg2 = (UINTN)Size;
    SmcCmd.Arg3 = (UINTN)State;

    ArmCallSmc (&SmcCmd);
    Status = RsiCmdStatusToEfiStatus (SmcCmd.Arg0);
    if (RETURN_ERROR (Status)) {
      break;
    }

    BaseAddress = (UINT64 *)SmcCmd.Arg1;
    Size        = EndAddress - BaseAddress;
  }   // while

  return Status;
}

/**
  Read the Realm Configuration.

  @param [out]  Config     Pointer to the address of the buffer to retrieve
                           the Realm configuration.

  Note: The buffer to retrieve the Realm configuration must be aligned to the
        Realm granule size.

  @retval RETURN_SUCCESS            Success.
  @retval RETURN_INVALID_PARAMETER  A parameter is invalid.
**/
RETURN_STATUS
EFIAPI
RsiGetRealmConfig (
  OUT REALM_CONFIG  *Config
  )
{
  ARM_SMC_ARGS  SmcCmd;

  if ((Config == NULL) || (!IsGranuleAligned ((UINT64 *)Config))) {
    return RETURN_INVALID_PARAMETER;
  }

  ZeroMem (&SmcCmd, sizeof (SmcCmd));
  SmcCmd.Arg0 = FID_RSI_REALM_CONFIG;
  SmcCmd.Arg1 = (UINTN)Config;

  ArmCallSmc (&SmcCmd);
  return RsiCmdStatusToEfiStatus (SmcCmd.Arg0);
}

/**
   Get the version of the RSI implementation.

  @param [out] Major  The major version of the RSI implementation.
  @param [out] Minor  The minor version of the RSI implementation.

  @retval RETURN_SUCCESS            Success.
  @retval RETURN_INVALID_PARAMETER  A parameter is invalid.
**/
RETURN_STATUS
EFIAPI
RsiGetVersion (
  OUT UINT16 *CONST  Major,
  OUT UINT16 *CONST  Minor
  )
{
  ARM_SMC_ARGS  SmcCmd;

  if ((Major == NULL) || (Minor == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&SmcCmd, sizeof (SmcCmd));
  SmcCmd.Arg0 = FID_RSI_VERSION;

  ArmCallSmc (&SmcCmd);
  *Minor = SmcCmd.Arg0 & RSI_VER_MINOR_MASK;
  *Major = (SmcCmd.Arg0 & RSI_VER_MAJOR_MASK) >> RSI_VER_MAJOR_SHIFT;
  return RETURN_SUCCESS;
}
