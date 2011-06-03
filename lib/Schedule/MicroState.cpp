//===---- MicroState.cpp - Represent MicroState in a FSM state  --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//
//===----------------------------------------------------------------------===//

#include "vtm/MicroState.h"
#include "vtm/VerilogAST.h"
#include "vtm/VInstrInfo.h"
#include "vtm/VRegisterInfo.h"
#include "vtm/VTM.h"

#include "llvm/Function.h"
#include "llvm/Metadata.h"
#include "llvm/Type.h"
#include "llvm/Constants.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

bool ucOp::isControl() const {
  return OpCode.getParent()->getOpcode() == VTM::Control;
}

void ucOp::print(raw_ostream &OS) const {
  OS << OpCode.getDesc().getName();

  if (isControl()) {
    OS << " pred:[";
    getPredicate().print(OS);
    OS << ']';
  }

  OS << "{" << OpCode.getFUId() << "}"
     << "@" << OpCode.getPredSlot();
  OS << ' ';
  // Print the operands;
  for (op_iterator I = op_begin(), E = op_end(); I != E; ++I) {
    ucOperand &MOP = *I;
    MOP.print(OS);
    OS << ", ";
  }
}

void ucOp::dump() const {
  print(dbgs());
  dbgs() << '\n';
}

ucOp::op_iterator ucOpIterator::getNextIt() const {
  ucOp::op_iterator NextIt = CurIt;

  assert(ucOperand(*NextIt).isOpcode() && "Bad leading token");

  while (++NextIt != EndIt) {
    ucOperand &TokenOp = *NextIt;
    if (TokenOp.isOpcode()) break;
  }

  return NextIt;
}

// Out of line virtual function to provide home for the class.
void ucOp::anchor() {}

// Out of line virtual function to provide home for the class.
void ucOpIterator::anchor() {}

void ucState::print(raw_ostream &OS) const {
  switch (Instr.getOpcode()) {
  case VTM::Control:  OS << "Control ";   break;
  case VTM::Datapath: OS << "Datapath ";  break;
  default:  assert(0 && "Bad opcode!");   break;
  }

  OS << '@' << getSlot() << '\n';
  for (ucState::iterator I = begin(), E = end(); I != E; ++I) {
    (*I).print(OS.indent(2));
    OS << '\n';
  }
}

void ucState::dump() const {
  print(dbgs());
}

// Out of line virtual function to provide home for the class.
void ucState::anchor() {}


const TargetInstrDesc &ucOperand::getDesc() const {
  assert(isOpcode() && "getDesc on a wrong operand type!");
  const TargetMachine &TM = getParent()->getParent()->getParent()->getTarget();
  return TM.getInstrInfo()->get(getOpcode());
}

ucOp ucOperand::getucParent() {
  MachineInstr *MI = getParent();

  bool OpLocated = false;
  MachineInstr::mop_iterator IStart = MI->operands_begin(),
                             IEnd = MI->operands_end();
  for (MachineInstr::mop_iterator I = MI->operands_begin(),
       E = MI->operands_end(); I != E; ++I) {
    ucOperand &Op = cast<ucOperand>(*I);
    if (Op.isOpcode()) {
      if (!OpLocated) IStart = I;
      else {
        IEnd = I;
        break;
      }
    } else
      OpLocated = (&Op == this);
  }

  assert(OpLocated && "Broken MI found!");
  return ucOp(ucOp::op_iterator(IStart, ucOperand::Mapper()),
              ucOp::op_iterator(IEnd, ucOperand::Mapper()));
}

ucOperand ucOperand::CreateOpcode(unsigned Opcode, unsigned PredSlot,
                                  FuncUnitId FUId /*= VFUs::Trivial*/) {
  uint64_t Context = 0x0;
  Context |= (uint64_t(Opcode & OpcodeMask) << OpcodeShiftAmount);
  Context |= (uint64_t(PredSlot & PredSlotMask) << PredSlotShiftAmount);
  Context |= (uint64_t(FUId.getData() & FUIDMask) << FUIDShiftAmount);
  ucOperand MO = MachineOperand::CreateImm(Context);
  MO.setTargetFlags(IsOpcode);
  assert((MO.getOpcode() == Opcode && MO.getPredSlot() == PredSlot
          && MO.getFUId() == FUId) && "Data overflow!");
  return MO;
}

ucOperand ucOperand::CreateWireDefine(MachineRegisterInfo &MRI,
                                      unsigned BitWidth) {
  unsigned WireNum = MRI.createVirtualRegister(VTM::WireRegisterClass);
  ucOperand MO = MachineOperand::CreateReg(WireNum, true);
  MO.setBitWidth(BitWidth);
  MO.setIsWire();
  //MO.setIsEarlyClobber();
  return MO;
}

ucOperand ucOperand::CreateWireRead(unsigned WireNum, unsigned BitWidth) {
  ucOperand MO = MachineOperand::CreateReg(WireNum, false);
  MO.setBitWidth(BitWidth);
  MO.setIsWire();
  return MO;
}

ucOperand ucOperand::CreatePredicate(unsigned Reg) {
  // Read reg0 means always execute.
  if (Reg == 0) {
    // Create the default predicate operand, which means always execute.
    ucOperand MO = MachineOperand::CreateImm(1);
    MO.setBitWidth(1);
    return MO;
  }

  ucOperand MO = MachineOperand::CreateReg(Reg, false);
  MO.setBitWidth(1);
  return MO;
}

void ucOperand::print(raw_ostream &OS,
                      unsigned UB /* = 64 */, unsigned LB /* = 0 */) {
  switch (getType()) {
  case MachineOperand::MO_Register: {
    unsigned Reg = getReg();
    UB = std::min(getBitWidth(), UB);
    unsigned Offset = 0;
    if (TargetRegisterInfo::isVirtualRegister(Reg)) {
      if (isWire()) OS << "wire";
      else {
        MachineRegisterInfo &MRI
          = getParent()->getParent()->getParent()->getRegInfo();
        const TargetRegisterClass *RC = MRI.getRegClass(Reg);
        OS << "/*" << RC->getName() << "*/ ";
        OS << "reg";
      }
      OS << TargetRegisterInfo::virtReg2Index(Reg);
      OS << verilogBitRange(UB, LB, getBitWidth() != 1);
    } else {
      //assert(TargetRegisterInfo::isPhysicalRegister(Reg)
      //       && "Unexpected virtual register!");
      // Get the one of the 64 bit registers.
      OS << "/*reg" << Reg <<"*/ reg" << (Reg & ~0x7);
      // Select the sub register
      Offset = (Reg & 0x7) * 8;
      OS << verilogBitRange(UB + Offset, LB + Offset, true);
    }
    return;
  }
  case MachineOperand::MO_Immediate:
    OS << verilogConstToStr(getImm(), getBitWidth(), false);
    return;
  case MachineOperand::MO_ExternalSymbol:
    OS << getSymbolName();
    return;
  default:
    MachineOperand::print(OS);
  }
}

raw_ostream &llvm::printVMBB(raw_ostream &OS, const MachineBasicBlock &MBB) {
  OS << "Scheduled MBB: " << MBB.getName() << '\n';
  for (MachineBasicBlock::const_iterator I = MBB.begin(), E = MBB.end();
       I != E; ++I) {
    const MachineInstr *Instr = I;
    switch (Instr->getOpcode()) {
    case VTM::Control:
    case VTM::Datapath:
      ucState(*Instr).print(OS);
      break;
    default:
      OS << "OI: ";
      Instr->print(OS);
      break;
    }
  }
  OS << '\n';
  return OS;
}

raw_ostream &llvm::printVMF(raw_ostream &OS, const MachineFunction &MF) {
  OS << "Scheduled MF: " << MF.getFunction()->getName() << '\n';

  for (MachineFunction::const_iterator I = MF.begin(), E = MF.end();
       I != E; ++I)
    printVMBB(OS, *I);

  return OS;
}
