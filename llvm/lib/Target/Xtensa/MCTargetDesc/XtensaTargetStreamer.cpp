//===-- XtensaTargetStreamer.cpp - Xtensa Target Streamer Methods ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides Xtensa specific target streamer methods.
//
//===----------------------------------------------------------------------===//

#include "XtensaTargetStreamer.h"
#include "XtensaInstPrinter.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/Support/FormattedStream.h"

using namespace llvm;

XtensaTargetStreamer::XtensaTargetStreamer(MCStreamer &S)
    : MCTargetStreamer(S) {}

XtensaTargetAsmStreamer::XtensaTargetAsmStreamer(MCStreamer &S,
                                                 formatted_raw_ostream &OS)
    : XtensaTargetStreamer(S), OS(OS) {}

void XtensaTargetAsmStreamer::emitLiteral(std::string str) { OS << str; }

XtensaTargetELFStreamer::XtensaTargetELFStreamer(MCStreamer &S)
    : XtensaTargetStreamer(S) {}

void XtensaTargetELFStreamer::emitLiteralLabel(MCSymbol *LblSym, SMLoc L) {
  MCContext &Context = getStreamer().getContext();
  MCStreamer &OutStreamer = getStreamer();
  StringRef LiteralSectionPrefix = getLiteralSectionPrefix();
  std::string SectionName;

  if (LiteralSectionPrefix != "") {
    SectionName = LiteralSectionPrefix.str() + ".literal";
  } else {
    MCSectionELF *CS = (MCSectionELF *)OutStreamer.getCurrentSectionOnly();
    std::string CSectionName = CS->getName().str();
    std::size_t Pos = CSectionName.find(".text");
    if (Pos != std::string::npos) {
      SectionName = ".literal";
      SectionName += CSectionName.substr(Pos);
    } else {
      SectionName = CSectionName;
      SectionName += ".literal";
    }
  }

  MCSection *ConstSection = Context.getELFSection(
      SectionName, ELF::SHT_PROGBITS, ELF::SHF_EXECINSTR | ELF::SHF_ALLOC);
  ConstSection->setAlignment(Align(4));

  OutStreamer.pushSection();
  OutStreamer.switchSection(ConstSection);
  OutStreamer.emitLabel(LblSym, L);
  OutStreamer.popSection();
}

void XtensaTargetELFStreamer::emitLiteral(MCSymbol *LblSym, const MCExpr *Value,
                                          SMLoc L) {
  MCStreamer &OutStreamer = getStreamer();

  OutStreamer.emitLabel(LblSym, L);
  OutStreamer.emitValue(Value, 4, L);
}

void XtensaTargetELFStreamer::emitLiteral(const MCExpr *Value, SMLoc L) {
  MCContext &Context = getStreamer().getContext();
  MCStreamer &OutStreamer = getStreamer();
  MCSectionELF *CS = (MCSectionELF *)OutStreamer.getCurrentSectionOnly();
  StringRef LiteralSectionPrefix = getLiteralSectionPrefix();
  std::string SectionName;

  if (LiteralSectionPrefix != "") {
    SectionName = LiteralSectionPrefix.str() + ".literal";
  } else {
    std::string CSectionName = CS->getName().str();
    std::size_t Pos = CSectionName.find(".text");
    if (Pos != std::string::npos) {
      SectionName = ".literal";
      SectionName += CSectionName.substr(Pos);
    } else {
      SectionName = CSectionName;
      SectionName += ".literal";
    }
  }

  MCSection *ConstSection = Context.getELFSection(
      SectionName, ELF::SHT_PROGBITS, ELF::SHF_EXECINSTR | ELF::SHF_ALLOC);

  OutStreamer.pushSection();
  OutStreamer.switchSection(ConstSection);
  OutStreamer.emitValue(Value, 4, L);
  OutStreamer.popSection();
}

MCELFStreamer &XtensaTargetELFStreamer::getStreamer() {
  return static_cast<MCELFStreamer &>(Streamer);
}
