//===--- XtensaUtils.cpp ---- Xtensa Utility Functions ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains miscellaneous utility functions.
//
//===----------------------------------------------------------------------===//

#include "XtensaUtils.h"

namespace llvm {

bool isValidAddrOffset(int Scale, int64_t OffsetVal) {
  bool Valid = false;

  switch (Scale) {
  case 1:
    Valid = (OffsetVal >= 0 && OffsetVal <= 255);
    break;
  case 2:
    Valid = (OffsetVal >= 0 && OffsetVal <= 510) && ((OffsetVal & 0x1) == 0);
    break;
  case 4:
    Valid = (OffsetVal >= 0 && OffsetVal <= 1020) && ((OffsetVal & 0x3) == 0);
    break;
  default:
    break;
  }
  return Valid;
}

bool isValidAddrOffset(MachineInstr &MI, int64_t Offset) {
  int Scale = 0;

  switch (MI.getOpcode()) {
  case Xtensa::L8I_P:
  case Xtensa::L8UI:
  case Xtensa::S8I:
    Scale = 1;
    break;
  case Xtensa::L16SI:
  case Xtensa::L16UI:
  case Xtensa::S16I:
    Scale = 2;
    break;
  case Xtensa::LEA_ADD:
    return (Offset >= -128 && Offset <= 127);
  case Xtensa::AE_L64_I:
  case Xtensa::AE_S64_I:
  case Xtensa::AE_S32X2_I:
  case Xtensa::AE_L32X2_I:
  case Xtensa::AE_S16X4_I:
  case Xtensa::AE_L16X4_I:
  case Xtensa::AE_LALIGN64_I:
  case Xtensa::AE_SALIGN64_I:
    return (Offset >= -64 && Offset <= 56);
  case Xtensa::AE_S64_IP:
  case Xtensa::AE_L64_IP:
  case Xtensa::AE_S32X2_IP:
  case Xtensa::AE_L32X2_IP:
  case Xtensa::AE_S16X4_IP:
  case Xtensa::AE_L16X4_IP:
    return (Offset >= 0 && Offset <= 56);
  case Xtensa::AE_L16X2M_I:
  case Xtensa::AE_L16X2M_IU:
  case Xtensa::AE_L32F24_I:
  case Xtensa::AE_L32F24_IP:
  case Xtensa::AE_L32M_I:
  case Xtensa::AE_L32M_IU:
  case Xtensa::AE_L32_I:
  case Xtensa::AE_L32_IP:
  case Xtensa::AE_S16X2M_I:
  case Xtensa::AE_S16X2M_IU:
  case Xtensa::AE_S24RA64S_I:
  case Xtensa::AE_S24RA64S_IP:
  case Xtensa::AE_S32F24_L_I:
  case Xtensa::AE_S32F24_L_IP:
  case Xtensa::AE_S32M_I:
  case Xtensa::AE_S32M_IU:
  case Xtensa::AE_S32RA64S_I:
  case Xtensa::AE_S32RA64S_IP:
  case Xtensa::AE_S32_L_I:
  case Xtensa::AE_S32_L_IP:
    return (Offset >= -32 && Offset <= 28);
  default:
    // assume that MI is 32-bit load/store operation
    Scale = 4;
    break;
  }
  return isValidAddrOffset(Scale, Offset);
}

} // namespace llvm
