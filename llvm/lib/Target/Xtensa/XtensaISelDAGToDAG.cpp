//===- XtensaISelDAGToDAG.cpp - A dag to dag inst selector for Xtensa -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the Xtensa target.
//
//===----------------------------------------------------------------------===//

#include "Xtensa.h"
#include "XtensaTargetMachine.h"
#include "XtensaUtils.h"
#include "llvm/IR/IntrinsicsXtensa.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "xtensa-isel"

namespace {

class XtensaDAGToDAGISel : public SelectionDAGISel {
  const XtensaSubtarget *Subtarget;
public:
  XtensaDAGToDAGISel(XtensaTargetMachine &TM, CodeGenOptLevel OptLevel)
      : SelectionDAGISel(TM, OptLevel), Subtarget(nullptr) {}

  void Select(SDNode *Node) override;

  bool SelectInlineAsmMemoryOperand(const SDValue &Op,
                                    InlineAsm::ConstraintCode ConstraintID,
                                    std::vector<SDValue> &OutOps) override;

  // For load/store instructions generate (base+offset) pair from
  // memory address. The offset must be a multiple of scale argument.
  bool selectMemRegAddr(SDValue Addr, SDValue &Base, SDValue &Offset,
                        int Scale) {
    EVT ValTy = Addr.getValueType();

    // if Address is FI, get the TargetFrameIndex.
    if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(Addr)) {
      Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), ValTy);
      Offset = CurDAG->getTargetConstant(0, SDLoc(Addr), ValTy);

      return true;
    }

    if (TM.isPositionIndependent()) {
      DiagnosticInfoUnsupported Diag(CurDAG->getMachineFunction().getFunction(),
                                     "PIC relocations are not supported ",
                                     Addr.getDebugLoc());
      CurDAG->getContext()->diagnose(Diag);
    }

    if ((Addr.getOpcode() == ISD::TargetExternalSymbol ||
         Addr.getOpcode() == ISD::TargetGlobalAddress))
      return false;

    // Addresses of the form FI+const
    bool Valid = false;
    if (CurDAG->isBaseWithConstantOffset(Addr)) {
      ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1));
      int64_t OffsetVal = CN->getSExtValue();

      Valid = isValidAddrOffset(Scale, OffsetVal);

      if (Valid) {
        // If the first operand is a FI, get the TargetFI Node
        if (FrameIndexSDNode *FIN =
                dyn_cast<FrameIndexSDNode>(Addr.getOperand(0)))
          Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), ValTy);
        else
          Base = Addr.getOperand(0);

        Offset =
            CurDAG->getTargetConstant(CN->getZExtValue(), SDLoc(Addr), ValTy);
        return true;
      }
    }

    // Last case
    Base = Addr;
    Offset = CurDAG->getTargetConstant(0, SDLoc(Addr), Addr.getValueType());
    return true;
  }

  bool selectMemRegAddrISH1(SDValue Addr, SDValue &Base, SDValue &Offset) {
    return selectMemRegAddr(Addr, Base, Offset, 1);
  }

  bool selectMemRegAddrISH2(SDValue Addr, SDValue &Base, SDValue &Offset) {
    return selectMemRegAddr(Addr, Base, Offset, 2);
  }

  bool selectMemRegAddrISH4(SDValue Addr, SDValue &Base, SDValue &Offset) {
    return selectMemRegAddr(Addr, Base, Offset, 4);
  }

  bool runOnMachineFunction(MachineFunction &MF) {
    Subtarget = &MF.getSubtarget<XtensaSubtarget>();
    return SelectionDAGISel::runOnMachineFunction(MF);
  }

  template <signed Low, signed High, signed Scale>
  bool selectMemRegImm(SDValue Addr, SDValue &Base, SDValue &Offset) {
    EVT ValTy = Addr.getValueType();
    // if Address is FI, get the TargetFrameIndex.
    if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(Addr)) {
      Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), ValTy);
      Offset = CurDAG->getTargetConstant(0, SDLoc(Addr), ValTy);

      return true;
    }
    if (TM.isPositionIndependent())
      report_fatal_error("PIC relocations is not supported");

    if ((Addr.getOpcode() == ISD::TargetExternalSymbol ||
         Addr.getOpcode() == ISD::TargetGlobalAddress))
      return false;

    if (CurDAG->isBaseWithConstantOffset(Addr)) {
      ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1));
      int64_t OffsetVal = CN->getSExtValue();
      if (((OffsetVal % std::abs(Scale)) == 0) && (OffsetVal >= Low) &&
          (OffsetVal <= High)) {
        if (FrameIndexSDNode *FIN =
                dyn_cast<FrameIndexSDNode>(Addr.getOperand(0)))
          Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), ValTy);
        else
          Base = Addr.getOperand(0);
        Offset = CurDAG->getTargetConstant(OffsetVal, SDLoc(Addr),
                                           Addr.getValueType());
        return true;
      }
    }
    // Last case
    Base = Addr;
    Offset = CurDAG->getTargetConstant(0, SDLoc(Addr), Addr.getValueType());
    return true;
  }

// Include the pieces autogenerated from the target description.
#include "XtensaGenDAGISel.inc"
}; // namespace

class XtensaDAGToDAGISelLegacy : public SelectionDAGISelLegacy {
public:
  static char ID;

  XtensaDAGToDAGISelLegacy(XtensaTargetMachine &TM, CodeGenOptLevel OptLevel)
      : SelectionDAGISelLegacy(
            ID, std::make_unique<XtensaDAGToDAGISel>(TM, OptLevel)) {}

  StringRef getPassName() const override {
    return "Xtensa DAG->DAG Pattern Instruction Selection";
  }
};
} // end anonymous namespace

char XtensaDAGToDAGISelLegacy::ID = 0;

FunctionPass *llvm::createXtensaISelDag(XtensaTargetMachine &TM,
                                        CodeGenOptLevel OptLevel) {
  return new XtensaDAGToDAGISelLegacy(TM, OptLevel);
}

void XtensaDAGToDAGISel::Select(SDNode *Node) {
  SDLoc DL(Node);
  EVT VT = Node->getValueType(0);
  const unsigned MRTable[] = {Xtensa::M0, Xtensa::M1, Xtensa::M2, Xtensa::M3};

  // If we have a custom node, we already have selected!
  if (Node->isMachineOpcode()) {
    Node->setNodeId(-1);
    return;
  }

  switch (Node->getOpcode()) {
  case ISD::SHL: {
    SDValue N0 = Node->getOperand(0);
    SDValue N1 = Node->getOperand(1);
    auto *C = dyn_cast<ConstantSDNode>(N1);
    // If C is constant in range [1..31] then we can generate SLLI
    // instruction using pattern matching, otherwise generate SLL
    if (!C || !(isUInt<5>(C->getZExtValue()) && !C->isZero())) {
      SDNode *SSL = CurDAG->getMachineNode(Xtensa::SSL, DL, MVT::Glue, N1);
      SDNode *SLL =
          CurDAG->getMachineNode(Xtensa::SLL, DL, VT, N0, SDValue(SSL, 0));
      ReplaceNode(Node, SLL);
      return;
    }
    break;
  }
  case ISD::SRL: {
    SDValue N0 = Node->getOperand(0);
    SDValue N1 = Node->getOperand(1);
    auto *C = dyn_cast<ConstantSDNode>(N1);

    // If C is constant then we can generate SRLI
    // instruction using pattern matching or EXTUI, otherwise generate SRL
    if (C) {
      if (isUInt<4>(C->getZExtValue()))
        break;
      unsigned ShAmt = C->getZExtValue();
      SDNode *EXTUI = CurDAG->getMachineNode(
          Xtensa::EXTUI, DL, VT, N0, CurDAG->getTargetConstant(ShAmt, DL, VT),
          CurDAG->getTargetConstant(32 - ShAmt, DL, VT));
      ReplaceNode(Node, EXTUI);
      return;
    }

    SDNode *SSR = CurDAG->getMachineNode(Xtensa::SSR, DL, MVT::Glue, N1);
    SDNode *SRL =
        CurDAG->getMachineNode(Xtensa::SRL, DL, VT, N0, SDValue(SSR, 0));
    ReplaceNode(Node, SRL);
    return;
  }
  case ISD::SRA: {
    SDValue N0 = Node->getOperand(0);
    SDValue N1 = Node->getOperand(1);
    auto *C = dyn_cast<ConstantSDNode>(N1);
    // If C is constant then we can generate SRAI
    // instruction using pattern matching, otherwise generate SRA
    if (!C) {
      SDNode *SSR = CurDAG->getMachineNode(Xtensa::SSR, DL, MVT::Glue, N1);
      SDNode *SRA =
          CurDAG->getMachineNode(Xtensa::SRA, DL, VT, N0, SDValue(SSR, 0));
      ReplaceNode(Node, SRA);
      return;
    }
    break;
  }
  case XtensaISD::SRCL: {
    SDValue N0 = Node->getOperand(0);
    SDValue N1 = Node->getOperand(1);
    SDValue N2 = Node->getOperand(2);
    SDNode *SSL = CurDAG->getMachineNode(Xtensa::SSL, DL, MVT::Glue, N2);
    SDNode *SRC =
        CurDAG->getMachineNode(Xtensa::SRC, DL, VT, N0, N1, SDValue(SSL, 0));
    ReplaceNode(Node, SRC);
    return;
  }
  case XtensaISD::SRCR: {
    SDValue N0 = Node->getOperand(0);
    SDValue N1 = Node->getOperand(1);
    SDValue N2 = Node->getOperand(2);
    SDNode *SSR = CurDAG->getMachineNode(Xtensa::SSR, DL, MVT::Glue, N2);
    SDNode *SRC =
        CurDAG->getMachineNode(Xtensa::SRC, DL, VT, N0, N1, SDValue(SSR, 0));
    ReplaceNode(Node, SRC);
    return;
  }
  case ISD::INTRINSIC_VOID: {
    unsigned IntNo = cast<ConstantSDNode>(Node->getOperand(1))->getZExtValue();
    switch (IntNo) {
    default:
      break;
    case Intrinsic::xtensa_mul_da_ll:
    case Intrinsic::xtensa_mul_da_lh:
    case Intrinsic::xtensa_mul_da_hl:
    case Intrinsic::xtensa_mul_da_hh:
    case Intrinsic::xtensa_mula_da_ll:
    case Intrinsic::xtensa_mula_da_lh:
    case Intrinsic::xtensa_mula_da_hl:
    case Intrinsic::xtensa_mula_da_hh:
    case Intrinsic::xtensa_muls_da_ll:
    case Intrinsic::xtensa_muls_da_lh:
    case Intrinsic::xtensa_muls_da_hl:
    case Intrinsic::xtensa_muls_da_hh: {
      SDValue ChainIn = Node->getOperand(0);
      SDValue ValueMX = Node->getOperand(2);
      SDValue ValueT = Node->getOperand(3);
      unsigned OpCode;

      switch (IntNo) {
      case Intrinsic::xtensa_mul_da_ll:
        OpCode = Xtensa::MUL_DA_LL;
        break;
      case Intrinsic::xtensa_mul_da_lh:
        OpCode = Xtensa::MUL_DA_LH;
        break;
      case Intrinsic::xtensa_mul_da_hl:
        OpCode = Xtensa::MUL_DA_HL;
        break;
      case Intrinsic::xtensa_mul_da_hh:
        OpCode = Xtensa::MUL_DA_HH;
        break;
      case Intrinsic::xtensa_mula_da_ll:
        OpCode = Xtensa::MULA_DA_LL;
        break;
      case Intrinsic::xtensa_mula_da_lh:
        OpCode = Xtensa::MULA_DA_LH;
        break;
      case Intrinsic::xtensa_mula_da_hl:
        OpCode = Xtensa::MULA_DA_HL;
        break;
      case Intrinsic::xtensa_mula_da_hh:
        OpCode = Xtensa::MULA_DA_HH;
        break;
      case Intrinsic::xtensa_muls_da_ll:
        OpCode = Xtensa::MULS_DA_LL;
        break;
      case Intrinsic::xtensa_muls_da_lh:
        OpCode = Xtensa::MULS_DA_LH;
        break;
      case Intrinsic::xtensa_muls_da_hl:
        OpCode = Xtensa::MULS_DA_HL;
        break;
      case Intrinsic::xtensa_muls_da_hh:
        OpCode = Xtensa::MULS_DA_HH;
        break;
      }

      uint64_t MXVal = 4;
      if (ValueMX.getOpcode() == ISD::TargetConstant) {
        MXVal = cast<ConstantSDNode>(ValueMX)->getZExtValue();
      }

      assert(
          (MXVal < 2) &&
          "Unexpected value of mul*_da* first argument, it must be m0 or m1");
      unsigned MXReg = MRTable[MXVal];

      const EVT MULAResTys[] = {MVT::Other};
      SmallVector<SDValue, 2> MULAOps;
      MULAOps.push_back(CurDAG->getRegister(MXReg, MVT::i32));
      MULAOps.push_back(ValueT);
      MULAOps.push_back(ChainIn);

      SDNode *MULA = CurDAG->getMachineNode(OpCode, DL, MULAResTys, MULAOps);
      ReplaceNode(Node, MULA);
      return;
    }
    case Intrinsic::xtensa_mul_ad_ll:
    case Intrinsic::xtensa_mul_ad_lh:
    case Intrinsic::xtensa_mul_ad_hl:
    case Intrinsic::xtensa_mul_ad_hh:
    case Intrinsic::xtensa_mula_ad_ll:
    case Intrinsic::xtensa_mula_ad_lh:
    case Intrinsic::xtensa_mula_ad_hl:
    case Intrinsic::xtensa_mula_ad_hh:
    case Intrinsic::xtensa_muls_ad_ll:
    case Intrinsic::xtensa_muls_ad_lh:
    case Intrinsic::xtensa_muls_ad_hl:
    case Intrinsic::xtensa_muls_ad_hh: {
      SDValue ChainIn = Node->getOperand(0);
      SDValue ValueS = Node->getOperand(2);
      SDValue ValueMY = Node->getOperand(3);
      unsigned OpCode;

      switch (IntNo) {
      case Intrinsic::xtensa_mul_ad_ll:
        OpCode = Xtensa::MUL_AD_LL;
        break;
      case Intrinsic::xtensa_mul_ad_lh:
        OpCode = Xtensa::MUL_AD_LH;
        break;
      case Intrinsic::xtensa_mul_ad_hl:
        OpCode = Xtensa::MUL_AD_HL;
        break;
      case Intrinsic::xtensa_mul_ad_hh:
        OpCode = Xtensa::MUL_AD_HH;
        break;
      case Intrinsic::xtensa_mula_ad_ll:
        OpCode = Xtensa::MULA_AD_LL;
        break;
      case Intrinsic::xtensa_mula_ad_lh:
        OpCode = Xtensa::MULA_AD_LH;
        break;
      case Intrinsic::xtensa_mula_ad_hl:
        OpCode = Xtensa::MULA_AD_HL;
        break;
      case Intrinsic::xtensa_mula_ad_hh:
        OpCode = Xtensa::MULA_AD_HH;
        break;
      case Intrinsic::xtensa_muls_ad_ll:
        OpCode = Xtensa::MULS_AD_LL;
        break;
      case Intrinsic::xtensa_muls_ad_lh:
        OpCode = Xtensa::MULS_AD_LH;
        break;
      case Intrinsic::xtensa_muls_ad_hl:
        OpCode = Xtensa::MULS_AD_HL;
        break;
      case Intrinsic::xtensa_muls_ad_hh:
        OpCode = Xtensa::MULS_AD_HH;
        break;
      }

      uint64_t MYVal = 4;
      if (ValueMY.getOpcode() == ISD::TargetConstant) {
        MYVal = cast<ConstantSDNode>(ValueMY)->getZExtValue();
      }

      assert(
          ((MYVal > 1) && (MYVal < 4)) &&
          "Unexpected value of mul*_ad* second argument, it must be m2 or m3");
      unsigned MYReg = MRTable[MYVal];

      const EVT MULAResTys[] = {MVT::Other};
      SmallVector<SDValue, 2> MULAOps;
      MULAOps.push_back(ValueS);
      MULAOps.push_back(CurDAG->getRegister(MYReg, MVT::i32));
      MULAOps.push_back(ChainIn);

      SDNode *MULA = CurDAG->getMachineNode(OpCode, DL, MULAResTys, MULAOps);
      ReplaceNode(Node, MULA);
      return;
    }
    case Intrinsic::xtensa_mul_dd_ll:
    case Intrinsic::xtensa_mul_dd_lh:
    case Intrinsic::xtensa_mul_dd_hl:
    case Intrinsic::xtensa_mul_dd_hh:
    case Intrinsic::xtensa_mula_dd_ll:
    case Intrinsic::xtensa_mula_dd_lh:
    case Intrinsic::xtensa_mula_dd_hl:
    case Intrinsic::xtensa_mula_dd_hh:
    case Intrinsic::xtensa_muls_dd_ll:
    case Intrinsic::xtensa_muls_dd_lh:
    case Intrinsic::xtensa_muls_dd_hl:
    case Intrinsic::xtensa_muls_dd_hh: {
      SDValue ChainIn = Node->getOperand(0);
      SDValue ValueMX = Node->getOperand(2);
      SDValue ValueMY = Node->getOperand(3);
      unsigned OpCode;

      switch (IntNo) {
      case Intrinsic::xtensa_mul_dd_ll:
        OpCode = Xtensa::MUL_DD_LL;
        break;
      case Intrinsic::xtensa_mul_dd_lh:
        OpCode = Xtensa::MUL_DD_LH;
        break;
      case Intrinsic::xtensa_mul_dd_hl:
        OpCode = Xtensa::MUL_DD_HL;
        break;
      case Intrinsic::xtensa_mul_dd_hh:
        OpCode = Xtensa::MUL_DD_HH;
        break;
      case Intrinsic::xtensa_mula_dd_ll:
        OpCode = Xtensa::MULA_DD_LL;
        break;
      case Intrinsic::xtensa_mula_dd_lh:
        OpCode = Xtensa::MULA_DD_LH;
        break;
      case Intrinsic::xtensa_mula_dd_hl:
        OpCode = Xtensa::MULA_DD_HL;
        break;
      case Intrinsic::xtensa_mula_dd_hh:
        OpCode = Xtensa::MULA_DD_HH;
        break;
      case Intrinsic::xtensa_muls_dd_ll:
        OpCode = Xtensa::MULS_DD_LL;
        break;
      case Intrinsic::xtensa_muls_dd_lh:
        OpCode = Xtensa::MULS_DD_LH;
        break;
      case Intrinsic::xtensa_muls_dd_hl:
        OpCode = Xtensa::MULS_DD_HL;
        break;
      case Intrinsic::xtensa_muls_dd_hh:
        OpCode = Xtensa::MULS_DD_HH;
        break;
      }
      uint64_t MXVal = 4;
      if (ValueMX.getOpcode() == ISD::TargetConstant) {
        MXVal = cast<ConstantSDNode>(ValueMX)->getZExtValue();
      }

      assert(
          (MXVal < 2) &&
          "Unexpected value of mul*_dd* first argument, it must be m0 or m1");
      unsigned MXReg = MRTable[MXVal];

      uint64_t MYVal = 4;
      if (ValueMY.getOpcode() == ISD::TargetConstant) {
        MYVal = cast<ConstantSDNode>(ValueMY)->getZExtValue();
      }

      assert(
          ((MYVal > 1) && (MYVal < 4)) &&
          "Unexpected value of mul*_dd* second argument, it must be m2 or m3");
      unsigned MYReg = MRTable[MYVal];

      const EVT MULAResTys[] = {MVT::Other};
      SmallVector<SDValue, 2> MULAOps;
      MULAOps.push_back(CurDAG->getRegister(MXReg, MVT::i32));
      MULAOps.push_back(CurDAG->getRegister(MYReg, MVT::i32));
      MULAOps.push_back(ChainIn);

      SDNode *MULA = CurDAG->getMachineNode(OpCode, DL, MULAResTys, MULAOps);
      ReplaceNode(Node, MULA);
      return;
    }
    }
    break;
  }

  case ISD::INTRINSIC_W_CHAIN: {
    unsigned IntNo = cast<ConstantSDNode>(Node->getOperand(1))->getZExtValue();
    unsigned OpCode = 0;
    bool Skip = false;

    switch (IntNo) {
    default:
      Skip = true;
      break;
    case Intrinsic::xtensa_xt_lsxp:
      OpCode = Xtensa::LSXP;
      break;
    case Intrinsic::xtensa_xt_lsip:
      OpCode = Xtensa::LSIP;
      break;
    }
    if (Skip)
      break;

    SDValue Chain = Node->getOperand(0);

    auto ResTys = Node->getVTList();

    SmallVector<SDValue, 5> Ops;
    for (unsigned i = 2; i < Node->getNumOperands(); i++)
      Ops.push_back(Node->getOperand(i));
    Ops.push_back(Chain);

    SDNode *NewNode = CurDAG->getMachineNode(OpCode, DL, ResTys, Ops);

    ReplaceNode(Node, NewNode);
    return;
  }

  default:
    break;
  }

  SelectCode(Node);
}

bool XtensaDAGToDAGISel::SelectInlineAsmMemoryOperand(
    const SDValue &Op, InlineAsm::ConstraintCode ConstraintID,
    std::vector<SDValue> &OutOps) {
  switch (ConstraintID) {
  default:
    llvm_unreachable("Unexpected asm memory constraint");
  case InlineAsm::ConstraintCode::m: {
    SDValue Base, Offset;
    // TODO
    selectMemRegAddr(Op, Base, Offset, 4);
    OutOps.push_back(Base);
    OutOps.push_back(Offset);
    return false;
  }
  case InlineAsm::ConstraintCode::i:
  case InlineAsm::ConstraintCode::R:
  case InlineAsm::ConstraintCode::ZC:
    OutOps.push_back(Op);
    return false;
  }
  return false;
}
