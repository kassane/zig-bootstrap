//===- XtensaFrameLowering.cpp - Xtensa Frame Information -----------------===//
//
//                     The LLVM Compiler Infrastructure
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

XtensaFrameLowering::XtensaFrameLowering()
    : TargetFrameLowering(TargetFrameLowering::StackGrowsDown, Align(4), 0,
                          Align(4)) {}

/*   Xtensa stack frames look like:

    +-------------------------------+
    |  incoming stack arguments     |
    +-------------------------------+
  A |  caller-allocated save area   |
    |  for register arguments       |
    +-------------------------------+ <-- incoming stack pointer
  B | CALL0 ABI:                    |
    |  callee-allocated save area   |
    |  for arguments that are       |
    |  split between registers and  |
    |  the stack (Register-Spill    |
    |  Area)                        |
    |                               |
    | Win ABI:                      |
    |  Register-Spill Overflow      |
    |  8 words for CALL8/CALLX8     |
    +-------------------------------+ <-- arg_pointer_rtx
  C |  callee-allocated save area   |
    |  for register varargs         |
    +-------------------------------+ <-- hard_frame_pointer_rtx;
    |                               |     stack_pointer_rtx + gp_sp_offset
    |  GPR save area                |       + UNITS_PER_WORD
    +-------------------------------+ <-- stack_pointer_rtx + fp_sp_offset
    |                               |       + UNITS_PER_HWVALUE
    |  FPR save area                |
    +-------------------------------+ <-- frame_pointer_rtx (virtual)
    |  local variables              |
  P +-------------------------------+
    |  outgoing stack arguments     |
    +-------------------------------+
    |  caller-allocated save area   |
    |  for register arguments       |
    +-------------------------------+ <-- stack_pointer_rtx

   At least two of A, B and C will be empty.

   Dynamic stack allocations such as alloca insert data at point P.
   They decrease stack_pointer_rtx but leave frame_pointer_rtx and
   hard_frame_pointer_rtx unchanged.  */

// hasFP - Return true if the specified function should have a dedicated frame
// pointer register.  This is true if the function has variable sized allocas or
// if frame pointer elimination is disabled.
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
  const XtensaRegisterInfo *RegInfo = static_cast<const XtensaRegisterInfo *>(
      MF.getSubtarget().getRegisterInfo());
  const XtensaInstrInfo &TII =
      *static_cast<const XtensaInstrInfo *>(MF.getSubtarget().getInstrInfo());
  MachineBasicBlock::iterator MBBI = MBB.begin();
  const XtensaSubtarget &STI = MF.getSubtarget<XtensaSubtarget>();
  DebugLoc dl = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();
  unsigned SP = Xtensa::SP;
  unsigned FP = RegInfo->getFrameRegister(MF);
  MachineModuleInfo &MMI = MF.getMMI();
  const MCRegisterInfo *MRI = MMI.getContext().getRegisterInfo();
  XtensaFunctionInfo *XtensaFI = MF.getInfo<XtensaFunctionInfo>();

  // First, compute final stack size.
  uint64_t StackSize = MFI.getStackSize();
  uint64_t PrevStackSize = StackSize;

  // Round up StackSize to 16*N
  StackSize += (16 - StackSize) & 0xf;

  if (STI.isWinABI()) {
    StackSize += 32;

    if (StackSize <= 32760) {
      BuildMI(MBB, MBBI, dl, TII.get(Xtensa::ENTRY))
          .addReg(SP)
          .addImm(StackSize);
    } else {
      /* Use a8 as a temporary since a0-a7 may be live.  */
      unsigned TmpReg = Xtensa::A8;

      const XtensaInstrInfo &TII = *static_cast<const XtensaInstrInfo *>(
          MBB.getParent()->getSubtarget().getInstrInfo());
      BuildMI(MBB, MBBI, dl, TII.get(Xtensa::ENTRY))
          .addReg(SP)
          .addImm(MIN_FRAME_SIZE);
      TII.loadImmediate(MBB, MBBI, &TmpReg, StackSize - MIN_FRAME_SIZE);
      BuildMI(MBB, MBBI, dl, TII.get(Xtensa::SUB), TmpReg)
          .addReg(SP)
          .addReg(TmpReg);
      BuildMI(MBB, MBBI, dl, TII.get(Xtensa::MOVSP), SP).addReg(TmpReg);
    }

    // Store FP register in A8, because FP may be used to pass function
    // arguments
    if (XtensaFI->isSaveFrameRegister()) {
      BuildMI(MBB, MBBI, dl, TII.get(Xtensa::OR), Xtensa::A8)
          .addReg(FP)
          .addReg(FP);
    }

    // if framepointer enabled, set it to point to the stack pointer.
    if (hasFP(MF)) {
      // Insert instruction "move $fp, $sp" at this location.
      BuildMI(MBB, MBBI, dl, TII.get(Xtensa::OR), FP)
          .addReg(SP)
          .addReg(SP)
          .setMIFlag(MachineInstr::FrameSetup);

      MCCFIInstruction Inst = MCCFIInstruction::cfiDefCfa(
          nullptr, MRI->getDwarfRegNum(FP, true), StackSize);
      unsigned CFIIndex = MF.addFrameInst(Inst);
      BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(CFIIndex);
    } else {
      // emit ".cfi_def_cfa_offset StackSize"
      unsigned CFIIndex = MF.addFrameInst(
          MCCFIInstruction::cfiDefCfaOffset(nullptr, StackSize));
      BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(CFIIndex);
    }
  } else {
    // No need to allocate space on the stack.
    if (StackSize == 0 && !MFI.adjustsStack())
      return;

    // Adjust stack.
    TII.adjustStackPtr(SP, -StackSize, MBB, MBBI);

    // emit ".cfi_def_cfa_offset StackSize"
    unsigned CFIIndex = MF.addFrameInst(
        MCCFIInstruction::cfiDefCfaOffset(nullptr, StackSize));
    BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
        .addCFIIndex(CFIIndex);

    const std::vector<CalleeSavedInfo> &CSI = MFI.getCalleeSavedInfo();

    if (CSI.size()) {
      // Find the instruction past the last instruction that saves a
      // callee-saved register to the stack.
      for (unsigned i = 0; i < CSI.size(); ++i)
        ++MBBI;

      // Iterate over list of callee-saved registers and emit .cfi_offset
      // directives.
      for (const auto &I : CSI) {
        int64_t Offset = MFI.getObjectOffset(I.getFrameIdx());
        unsigned Reg = I.getReg();

        unsigned CFIIndex = MF.addFrameInst(MCCFIInstruction::createOffset(
            nullptr, MRI->getDwarfRegNum(Reg, 1), Offset));
        BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
            .addCFIIndex(CFIIndex);
      }
    }

    // if framepointer enabled, set it to point to the stack pointer.
    if (hasFP(MF)) {
      // Insert instruction "move $fp, $sp" at this location.
      BuildMI(MBB, MBBI, dl, TII.get(Xtensa::OR), FP)
          .addReg(SP)
          .addReg(SP)
          .setMIFlag(MachineInstr::FrameSetup);

      // emit ".cfi_def_cfa_register $fp"
      unsigned CFIIndex =
          MF.addFrameInst(MCCFIInstruction::createDefCfaRegister(
              nullptr, MRI->getDwarfRegNum(FP, true)));
      BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
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
  const XtensaRegisterInfo *RegInfo = static_cast<const XtensaRegisterInfo *>(
      MF.getSubtarget().getRegisterInfo());
  const XtensaInstrInfo &TII =
      *static_cast<const XtensaInstrInfo *>(MF.getSubtarget().getInstrInfo());
  const XtensaSubtarget &STI = MF.getSubtarget<XtensaSubtarget>();
  DebugLoc dl = MBBI->getDebugLoc();
  unsigned SP = Xtensa::SP;
  unsigned FP = RegInfo->getFrameRegister(MF);

  // if framepointer enabled, restore the stack pointer.
  if (hasFP(MF)) {
    // Find the first instruction that restores a callee-saved register.
    MachineBasicBlock::iterator I = MBBI;

    for (unsigned i = 0; i < MFI.getCalleeSavedInfo().size(); ++i)
      --I;
    if (STI.isWinABI()) {
      // In most architectures, we need to explicitly restore the stack pointer
      // before returning.
      //
      // For Xtensa Windowed Register option, it is not needed to explicitly
      // restore the stack pointer. Reason being is that on function return,
      // the window of the caller (including the old stack pointer) gets
      // restored anyways.
    } else {
      BuildMI(MBB, I, dl, TII.get(Xtensa::OR), SP).addReg(FP).addReg(FP);
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
  const TargetInstrInfo &TII = *MF->getSubtarget().getInstrInfo();

  for (unsigned i = 0, e = CSI.size(); i != e; ++i) {
    // Add the callee-saved register as live-in. Do not add if the register is
    // A0 and return address is taken, because it will be implemented in
    // method XtensaTargetLowering::LowerRETURNADDR.
    // It's killed at the spill, unless the register is RA and return address
    // is taken.
    unsigned Reg = CSI[i].getReg();
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

    unsigned SP = Xtensa::SP;
    TII.adjustStackPtr(SP, Amount, MBB, I);
  }

  return MBB.erase(I);
}

void XtensaFrameLowering::determineCalleeSaves(MachineFunction &MF,
                                               BitVector &SavedRegs,
                                               RegScavenger *RS) const {
  const XtensaSubtarget &STI = MF.getSubtarget<XtensaSubtarget>();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  const XtensaRegisterInfo *RegInfo = static_cast<const XtensaRegisterInfo *>(
      MF.getSubtarget().getRegisterInfo());
  unsigned FP = RegInfo->getFrameRegister(MF);

  if (STI.isWinABI()) {
    return;
  }

  TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);

  // Mark $fp as used if function has dedicated frame pointer.
  if (hasFP(MF))
    SavedRegs.set(FP);

  // Set scavenging frame index if necessary.
  uint64_t MaxSPOffset = MFI.estimateStackSize(MF);

  if (isInt<12>(MaxSPOffset))
    return;

  const TargetRegisterClass &RC = Xtensa::ARRegClass;
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  unsigned Size = TRI->getSpillSize(RC);
  Align Alignment = TRI->getSpillAlign(RC);
  int FI = MF.getFrameInfo().CreateStackObject(Size, Alignment, false);
  RS->addScavengingFrameIndex(FI);
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
    for (int i = 0; i <= NeedRegs; i++)
      RS->addScavengingFrameIndex(
          MFI.CreateStackObject(Size, Alignment, false));
  }
}
