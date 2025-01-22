//===- XtensaFrameLowering.cpp - Xtensa Frame Information -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the Xtensa implementation of TargetFrameLowering class.
//
//===----------------------------------------------------------------------===//

#include "XtensaFrameLowering.h"
#include "XtensaInstrInfo.h"
#include "XtensaMachineFunctionInfo.h"
#include "XtensaSubtarget.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/IR/Function.h"

#define STACK_SIZE_THRESHOLD 100

using namespace llvm;

XtensaFrameLowering::XtensaFrameLowering(const XtensaSubtarget &STI)
    : TargetFrameLowering(TargetFrameLowering::StackGrowsDown, Align(4), 0,
                          Align(4)),
      TII(*STI.getInstrInfo()), TRI(STI.getRegisterInfo()) {}

bool XtensaFrameLowering::hasFP(const MachineFunction &MF) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  return MF.getTarget().Options.DisableFramePointerElim(MF) ||
         MFI.hasVarSizedObjects();
}

/* minimum frame = reg save area (4 words) plus static chain (1 word)
   and the total number of words must be a multiple of 128 bits.  */
/* Width of a word, in units (bytes).  */
#define UNITS_PER_WORD 4
#define MIN_FRAME_SIZE (8 * UNITS_PER_WORD)

void XtensaFrameLowering::emitPrologue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  assert(&MBB == &MF.front() && "Shrink-wrapping not yet implemented");
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineBasicBlock::iterator MBBI = MBB.begin();
  const XtensaSubtarget &STI = MF.getSubtarget<XtensaSubtarget>();
  DebugLoc DL = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();
  MCRegister SP = Xtensa::SP;
  MCRegister FP = TRI->getFrameRegister(MF);
  const MCRegisterInfo *MRI = MF.getContext().getRegisterInfo();
  XtensaFunctionInfo *XtensaFI = MF.getInfo<XtensaFunctionInfo>();

  // First, compute final stack size.
  uint64_t StackSize = MFI.getStackSize();
  uint64_t PrevStackSize = StackSize;

  // Round up StackSize to 16*N
  StackSize += (16 - StackSize) & 0xf;

  if (STI.isWinABI()) {
    StackSize += 32;

    if (StackSize <= 32760) {
      BuildMI(MBB, MBBI, DL, TII.get(Xtensa::ENTRY))
          .addReg(SP)
          .addImm(StackSize);
    } else {
      /* Use a8 as a temporary since a0-a7 may be live.  */
      unsigned TmpReg = Xtensa::A8;

      const XtensaInstrInfo &TII = *static_cast<const XtensaInstrInfo *>(
          MBB.getParent()->getSubtarget().getInstrInfo());
      BuildMI(MBB, MBBI, DL, TII.get(Xtensa::ENTRY))
          .addReg(SP)
          .addImm(MIN_FRAME_SIZE);
      TII.loadImmediate(MBB, MBBI, &TmpReg, StackSize - MIN_FRAME_SIZE);
      BuildMI(MBB, MBBI, DL, TII.get(Xtensa::SUB), TmpReg)
          .addReg(SP)
          .addReg(TmpReg);
      BuildMI(MBB, MBBI, DL, TII.get(Xtensa::MOVSP), SP).addReg(TmpReg);
    }

    // Store FP register in A8, because FP may be used to pass function
    // arguments
    if (XtensaFI->isSaveFrameRegister()) {
      BuildMI(MBB, MBBI, DL, TII.get(Xtensa::OR), Xtensa::A8)
          .addReg(FP)
          .addReg(FP);
    }

    // if framepointer enabled, set it to point to the stack pointer.
    if (hasFP(MF)) {
      // Insert instruction "move $fp, $sp" at this location.
      BuildMI(MBB, MBBI, DL, TII.get(Xtensa::OR), FP)
          .addReg(SP)
          .addReg(SP)
          .setMIFlag(MachineInstr::FrameSetup);

      MCCFIInstruction Inst = MCCFIInstruction::cfiDefCfa(
          nullptr, MRI->getDwarfRegNum(FP, true), StackSize);
      unsigned CFIIndex = MF.addFrameInst(Inst);
      BuildMI(MBB, MBBI, DL, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(CFIIndex);
    } else {
      // emit ".cfi_def_cfa_offset StackSize"
      unsigned CFIIndex = MF.addFrameInst(
          MCCFIInstruction::cfiDefCfaOffset(nullptr, StackSize));
      BuildMI(MBB, MBBI, DL, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(CFIIndex);
    }
  } else {
    // No need to allocate space on the stack.
    if (StackSize == 0 && !MFI.adjustsStack())
      return;

    // Adjust stack.
    TII.adjustStackPtr(SP, -StackSize, MBB, MBBI);

    // emit ".cfi_def_cfa_offset StackSize"
    unsigned CFIIndex =
        MF.addFrameInst(MCCFIInstruction::cfiDefCfaOffset(nullptr, StackSize));
    BuildMI(MBB, MBBI, DL, TII.get(TargetOpcode::CFI_INSTRUCTION))
        .addCFIIndex(CFIIndex);

    const std::vector<CalleeSavedInfo> &CSI = MFI.getCalleeSavedInfo();

    if (!CSI.empty()) {
      // Find the instruction past the last instruction that saves a
      // callee-saved register to the stack. The callee-saved store
      // instructions are placed at the begin of basic block, so
      //  iterate over instruction sequence and check that
      // save instructions are placed correctly.
      for (unsigned i = 0, e = CSI.size(); i < e; ++i) {
#ifndef NDEBUG
        const CalleeSavedInfo &Info = CSI[i];
        int FI = Info.getFrameIdx();
        int StoreFI = 0;

        // Checking that the instruction is exactly as expected
        bool IsStoreInst = false;
        if (MBBI->getOpcode() == TargetOpcode::COPY && Info.isSpilledToReg()) {
          Register DstReg = MBBI->getOperand(0).getReg();
          Register Reg = MBBI->getOperand(1).getReg();
          IsStoreInst = (Info.getDstReg() == DstReg) && (Info.getReg() == Reg);
        } else {
          Register Reg = TII.isStoreToStackSlot(*MBBI, StoreFI);
          IsStoreInst = (Reg == Info.getReg()) && (StoreFI == FI);
        }
        assert(IsStoreInst &&
               "Unexpected callee-saved register store instruction");
#endif
        ++MBBI;
      }

      // Iterate over list of callee-saved registers and emit .cfi_offset
      // directives.
      for (const auto &I : CSI) {
        int64_t Offset = MFI.getObjectOffset(I.getFrameIdx());
        Register Reg = I.getReg();

        unsigned CFIIndex = MF.addFrameInst(MCCFIInstruction::createOffset(
            nullptr, MRI->getDwarfRegNum(Reg, 1), Offset));
        BuildMI(MBB, MBBI, DL, TII.get(TargetOpcode::CFI_INSTRUCTION))
            .addCFIIndex(CFIIndex);
      }
    }

    // if framepointer enabled, set it to point to the stack pointer.
    if (hasFP(MF)) {
      // Insert instruction "move $fp, $sp" at this location.
      BuildMI(MBB, MBBI, DL, TII.get(Xtensa::OR), FP)
          .addReg(SP)
          .addReg(SP)
          .setMIFlag(MachineInstr::FrameSetup);

      // emit ".cfi_def_cfa_register $fp"
      unsigned CFIIndex =
          MF.addFrameInst(MCCFIInstruction::createDefCfaRegister(
              nullptr, MRI->getDwarfRegNum(FP, true)));
      BuildMI(MBB, MBBI, DL, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(CFIIndex);
    }
  }

  if (StackSize != PrevStackSize) {
    MFI.setStackSize(StackSize);

    for (int i = MFI.getObjectIndexBegin(); i < MFI.getObjectIndexEnd(); i++) {
      if (!MFI.isDeadObjectIndex(i)) {
        int64_t SPOffset = MFI.getObjectOffset(i);

        if (SPOffset < 0)
          MFI.setObjectOffset(i, SPOffset - StackSize + PrevStackSize);
      }
    }
  }
}

void XtensaFrameLowering::emitEpilogue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  MachineBasicBlock::iterator MBBI = MBB.getLastNonDebugInstr();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  const XtensaSubtarget &STI = MF.getSubtarget<XtensaSubtarget>();
  DebugLoc DL = MBBI->getDebugLoc();
  MCRegister SP = Xtensa::SP;
  MCRegister FP = TRI->getFrameRegister(MF);

  // if framepointer enabled, restore the stack pointer.
  if (hasFP(MF)) {
    // We should place restore stack pointer instruction just before
    // sequence of instructions which restores callee-saved registers.
    // This sequence is placed at the end of the basic block,
    // so we should find first instruction of the sequence.
    MachineBasicBlock::iterator I = MBBI;

    const std::vector<CalleeSavedInfo> &CSI = MFI.getCalleeSavedInfo();

    // Find the first instruction at the end that restores a callee-saved
    // register.
    for (unsigned i = 0, e = CSI.size(); i < e; ++i) {
      --I;
#ifndef NDEBUG
      const CalleeSavedInfo &Info = CSI[i];
      int FI = Info.getFrameIdx();
      int LoadFI = 0;

      // Checking that the instruction is exactly as expected
      bool IsRestoreInst = false;
      if (I->getOpcode() == TargetOpcode::COPY && Info.isSpilledToReg()) {
        Register Reg = I->getOperand(0).getReg();
        Register DstReg = I->getOperand(1).getReg();
        IsRestoreInst = (Info.getDstReg() == DstReg) && (Info.getReg() == Reg);
      } else {
        Register Reg = TII.isLoadFromStackSlot(*I, LoadFI);
        IsRestoreInst = (Info.getReg() == Reg) && (LoadFI == FI);
      }
      assert(IsRestoreInst &&
             "Unexpected callee-saved register restore instruction");
#endif
    }

    if (STI.isWinABI()) {
      // In most architectures, we need to explicitly restore the stack pointer
      // before returning.
      //
      // For Xtensa Windowed Register option, it is not needed to explicitly
      // restore the stack pointer. Reason being is that on function return,
      // the window of the caller (including the old stack pointer) gets
      // restored anyways.
    } else {
      BuildMI(MBB, I, DL, TII.get(Xtensa::OR), SP).addReg(FP).addReg(FP);
    }
  }

  if (STI.isWinABI())
    return;

  // Get the number of bytes from FrameInfo
  uint64_t StackSize = MFI.getStackSize();

  if (!StackSize)
    return;

  // Adjust stack.
  TII.adjustStackPtr(SP, StackSize, MBB, MBBI);
}

bool XtensaFrameLowering::spillCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    ArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  MachineFunction *MF = MBB.getParent();
  const XtensaSubtarget &STI = MF->getSubtarget<XtensaSubtarget>();

  if (STI.isWinABI())
    return true;

  MachineBasicBlock &EntryBlock = *(MF->begin());

  for (unsigned i = 0, e = CSI.size(); i != e; ++i) {
    // Add the callee-saved register as live-in. Do not add if the register is
    // A0 and return address is taken, because it will be implemented in
    // method XtensaTargetLowering::LowerRETURNADDR.
    // It's killed at the spill, unless the register is RA and return address
    // is taken.
    Register Reg = CSI[i].getReg();
    bool IsA0AndRetAddrIsTaken =
        (Reg == Xtensa::A0) && MF->getFrameInfo().isReturnAddressTaken();
    if (!IsA0AndRetAddrIsTaken)
      EntryBlock.addLiveIn(Reg);

    // Insert the spill to the stack frame.
    bool IsKill = !IsA0AndRetAddrIsTaken;
    const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(Reg);
    TII.storeRegToStackSlot(EntryBlock, MI, Reg, IsKill, CSI[i].getFrameIdx(),
                            RC, TRI, Register());
  }

  return true;
}

bool XtensaFrameLowering::restoreCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    MutableArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  MachineFunction *MF = MBB.getParent();
  const XtensaSubtarget &STI = MF->getSubtarget<XtensaSubtarget>();
  if (STI.isWinABI())
    return true;
  return TargetFrameLowering::restoreCalleeSavedRegisters(MBB, MI, CSI, TRI);
}

// Eliminate ADJCALLSTACKDOWN, ADJCALLSTACKUP pseudo instructions
MachineBasicBlock::iterator XtensaFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator I) const {
  const XtensaInstrInfo &TII =
      *static_cast<const XtensaInstrInfo *>(MF.getSubtarget().getInstrInfo());

  if (!hasReservedCallFrame(MF)) {
    int64_t Amount = I->getOperand(0).getImm();

    if (I->getOpcode() == Xtensa::ADJCALLSTACKDOWN)
      Amount = -Amount;

    TII.adjustStackPtr(Xtensa::SP, Amount, MBB, I);
  }

  return MBB.erase(I);
}

void XtensaFrameLowering::determineCalleeSaves(MachineFunction &MF,
                                               BitVector &SavedRegs,
                                               RegScavenger *RS) const {
  const XtensaSubtarget &STI = MF.getSubtarget<XtensaSubtarget>();
  unsigned FP = TRI->getFrameRegister(MF);

  if (STI.isWinABI()) {
    return;
  }

  TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);

  // Mark $fp as used if function has dedicated frame pointer.
  if (hasFP(MF))
    SavedRegs.set(FP);
}

void XtensaFrameLowering::processFunctionBeforeFrameFinalized(
    MachineFunction &MF, RegScavenger *RS) const {
  const XtensaSubtarget &STI = MF.getSubtarget<XtensaSubtarget>();

  // Presence of SPILL_* pseudo-instructions requires spill slots
  int NeedRegs = 0;
  for (const MachineBasicBlock &MBB : MF) {
    for (const MachineInstr &MI : MBB) {
      unsigned Opcode = MI.getOpcode();
      if (Opcode == Xtensa::SPILL_BOOL)
        NeedRegs += 1;

      if (Opcode == Xtensa::RESTORE_BOOL)
        NeedRegs += 3;
    }
  }
  NeedRegs = std::min(16, NeedRegs);

  // In WinABI mode add register scavenging slot
  // FIXME: It may be posssible to add spill slot by more optimal way
  if (STI.isWinABI() &&
      ((MF.getFrameInfo().estimateStackSize(MF) > STACK_SIZE_THRESHOLD) ||
      (NeedRegs > 0))) {
    MachineFrameInfo &MFI = MF.getFrameInfo();
    const TargetRegisterClass &RC = Xtensa::ARRegClass;
    const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
    unsigned Size = TRI.getSpillSize(RC);
    Align Alignment = TRI.getSpillAlign(RC);
    // If NeedsRegs == 0, we still need a spill slot
    for (int i = 0; i <= NeedRegs; i++)
      RS->addScavengingFrameIndex(
          MFI.CreateStackObject(Size, Alignment, false));
  }
}
