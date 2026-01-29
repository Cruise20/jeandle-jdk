/*
 * Copyright (c) 2025, the Jeandle-JDK Authors. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include "jeandle/__llvmHeadersBegin__.hpp"
#include "llvm/ExecutionEngine/JITLink/x86_64.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Host.h"

#include "jeandle/jeandleAssembler.hpp"
#include "jeandle/jeandleCompilation.hpp"
#include "jeandle/jeandleRuntimeRoutine.hpp"

#include "jeandle/__hotspotHeadersBegin__.hpp"
#include "code/nativeInst.hpp"
#include "runtime/sharedRuntime.hpp"

#define __ _masm->

namespace {

class JeandleInstructionResolver : public JeandleCompilationResourceObj {
private:
  address _begin_address;
  int _total_size;
  std::vector<int> _inst_offsets;

public:
  JeandleInstructionResolver(const address begin_addr, int total_size,
                             const llvm::Triple triple = llvm::Triple("x86_64-unknown-linux-gnu")) :
                             _begin_address(begin_addr),
                             _total_size(total_size) {
    initialize_inst_offsets(triple);
  }

  // Given a arbitrary address within code section, find the beginning of the next instruction.
  address lookup_next_instruction(const address current_ptr) {
    assert(current_ptr >= _begin_address && current_ptr < _begin_address + _total_size, "invalid target address");

    uint32_t current_offset = static_cast<uint32_t>(current_ptr - _begin_address);

    auto it = std::upper_bound(_inst_offsets.begin(), _inst_offsets.end(), current_offset);
    assert(it != _inst_offsets.begin(), "current_offset must be after the first instruction");

    uint32_t next_offset;
    if (it == _inst_offsets.end()) {
        // The next instruction is the last one in the code section.
        next_offset = static_cast<uint32_t>(_total_size);
    } else {
        next_offset = *it;
    }

    address next_inst_address = _begin_address + next_offset;
    assert(next_inst_address > current_ptr, "next instruction must be after current position");

    return next_inst_address;
  }

private:

  void initialize_inst_offsets(const llvm::Triple triple) {
    // Initialize LLVM components.
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllDisassemblers();

    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
    guarantee(target, "Could not get a x86 target: %s", error.c_str());

    // Prepare context for disassembling.
    std::unique_ptr<llvm::MCRegisterInfo> mri(target->createMCRegInfo(triple));
    std::unique_ptr<llvm::MCAsmInfo> mai(target->createMCAsmInfo(*mri, triple, llvm::MCTargetOptions()));
    std::unique_ptr<llvm::MCInstrInfo> mii(target->createMCInstrInfo());
    std::unique_ptr<llvm::MCSubtargetInfo> sti(target->createMCSubtargetInfo(triple, "", ""));
    llvm::MCContext ctx(triple, mai.get(), mri.get(), sti.get());
    std::unique_ptr<llvm::MCDisassembler> disas(target->createMCDisassembler(*sti, ctx));

    // Scan the code section.
    int current_offset = 0;
    _inst_offsets.reserve(_total_size / 4); // Estimate the number of instructions.

    while (current_offset < _total_size) {
      _inst_offsets.push_back(current_offset);

      // Get the size of current instruction.
      llvm::MCInst inst;
      uint64_t inst_size = 0;
      address current_start = _begin_address + current_offset;
      llvm::ArrayRef<uint8_t> data(current_start, _total_size - current_offset);

      auto status = disas->getInstruction(inst, inst_size, data, reinterpret_cast<uint64_t>(current_start), llvm::nulls());

      if (status == llvm::MCDisassembler::Fail) {
        // Might be a nop.
        inst_size = 1;
      }

      current_offset += static_cast<int>(inst_size);
    }
  }
};

} // anonymous namespace

void JeandleAssembler::emit_static_call_stub(int inst_offset, CallSiteInfo* call) {
  assert(inst_offset >= 0, "invalid call instruction address");
  assert(call->type() == JeandleCompiledCall::STATIC_CALL, "legal call type");
  address call_address = __ addr_at(inst_offset);

  int stub_size = 28;
  address stub = __ start_a_stub(stub_size);
  JEANDLE_ERROR_ASSERT_AND_RET_VOID_ON_FAIL(stub != nullptr, "static call stub overflow");

  int start = __ offset();

  // FIXME: Whether we need alignment here?
  __ align(BytesPerWord, __ offset() + NativeMovConstReg::instruction_size + NativeCall::displacement_offset);
  __ relocate(static_stub_Relocation::spec(call_address));
  __ mov_metadata(rbx, (Metadata*)nullptr);
  assert(((__ offset() + 1) % BytesPerWord) == 0, "must be aligned");
  __ jump(RuntimeAddress(__ pc()));

  assert(__ offset() - start <= stub_size, "stub too big");
  __ end_a_stub();
}

void JeandleAssembler::patch_static_call_site(int inst_offset, CallSiteInfo* call) {
  assert(inst_offset >= 0, "invalid call instruction address");
  assert(call->type() == JeandleCompiledCall::STATIC_CALL, "legal call type");
  address call_address =  __ addr_at(inst_offset);

  // Set insts_end to where to patch.
  int insts_end_offset = __ code()->insts_end() - __ code()->insts_begin();
  __ code()->set_insts_end(call_address);

  // Patch.
  if (call->target() == SharedRuntime::get_resolve_opt_virtual_call_stub()) {
    __ call(AddressLiteral(call->target(), relocInfo::opt_virtual_call_type));
  } else {
    assert(call->target() == SharedRuntime::get_resolve_static_call_stub(), "illegal call target");
    __ call(AddressLiteral(call->target(), relocInfo::static_call_type));
  }
  assert(__ offset() % 4 == 0, "must be aligned for MT-safe patch");

  // Recover insts_end.
  __ code()->set_insts_end(__ code()->insts_begin() + insts_end_offset);
}

void JeandleAssembler::patch_stub_C_call_site(int inst_offset, CallSiteInfo* call) {
  // No need to patch stub C call site on x86.
}

void JeandleAssembler::patch_routine_call_site(int inst_offset, address target) {
  assert(inst_offset >= 0, "invalid operand address");

  address call_address = __ addr_at(inst_offset);

  // Set insts_end to where to patch.
  int insts_end_offset = __ code()->insts_end() - __ code()->insts_begin();
  __ code()->set_insts_end(call_address);

  // Patch.
  __ call(AddressLiteral(target, relocInfo::runtime_call_type));

  // Recover insts_end.
  __ code()->set_insts_end(__ code()->insts_begin() + insts_end_offset);
}

void JeandleAssembler::patch_ic_call_site(int inst_offset, CallSiteInfo* call) {
  assert(inst_offset >= 0, "invalid call instruction address");
  assert(call->type() == JeandleCompiledCall::DYNAMIC_CALL, "legal call type");

  address call_address =  __ addr_at(inst_offset);

  // Set insts_end to where to patch.
  int insts_end_offset = __ code()->insts_end() - __ code()->insts_begin();
  __ code()->set_insts_end(call_address);

  // Patch.
  __ ic_call(call->target());
  assert(__ offset() % 4 == 0, "must be aligned for MT-safe patch");

  // Recover insts_end.
  __ code()->set_insts_end(__ code()->insts_begin() + insts_end_offset);
}

void JeandleAssembler::patch_external_call_site(int inst_offset, CallSiteInfo* call) {
  assert(inst_offset >= 0, "invalid call instruction address");
  assert(call->type() == JeandleCompiledCall::EXTERNAL_CALL, "legal call type");

  // The following `set_insts_end` conflicts with code buffer expansion,
  // we need to confirm that stub code section has enough space before invoking `set_insts_end`.
  int required_space = __ max_trampoline_stub_size();
  if (__ code()->stubs()->maybe_expand_to_ensure_remaining(required_space)) {
    JEANDLE_ERROR_ASSERT_AND_RET_VOID_ON_FAIL(__ code()->blob() != nullptr, "trampoline stub overflow");
  }

  address call_address = __ addr_at(inst_offset);
#ifdef ASSERT
  NativeInstruction* ni = nativeInstruction_at(call_address);
  assert(ni->is_call(), "doesn't look like a call");
#endif // ASSERT

  // Set insts_end to where to patch.
  int insts_end_offset = __ code()->insts_end() - __ code()->insts_begin();
  __ code()->set_insts_end(call_address);

  // Patch.
  address tpc = __ trampoline_call(AddressLiteral(call->target(), relocInfo::none));
  JEANDLE_ERROR_ASSERT_AND_RET_VOID_ON_FAIL(tpc != nullptr, "trampoline stub overflow");

  // Recover insts_end.
  __ code()->set_insts_end(__ code()->insts_begin() + insts_end_offset);
}

void JeandleAssembler::emit_ic_check() {
  uint insts_size = __ code()->insts_size();
  if (UseCompressedClassPointers) {
    __ load_klass(rscratch1, j_rarg0, rscratch2);
    __ cmpptr(rax, rscratch1);
  } else {
    __ cmpptr(rax, Address(j_rarg0, oopDesc::klass_offset_in_bytes()));
  }

  __ jump_cc(Assembler::notEqual, RuntimeAddress(SharedRuntime::get_ic_miss_stub()));

  // Align to 8 byte.
  int nops_cnt = 8 - ((__ code()->insts_size() - insts_size) & 0x7);
  if (nops_cnt > 0)
    __ nop(nops_cnt);
}

void JeandleAssembler::emit_verified_entry() {
  // Emit a 5-bytes address nop for patching a jump instruction.
  __ addr_nop_5();
}

void JeandleAssembler::emit_clinit_barrier_on_entry(Klass* klass) {
  Label fallthrough;
  __ mov_metadata(rscratch1, klass);
  __ clinit_barrier(rscratch1, r15_thread, &fallthrough);
  __ jump(RuntimeAddress(SharedRuntime::get_handle_wrong_method_stub()));
  __ bind(fallthrough);
}

int JeandleAssembler::interior_entry_alignment() const {
  // Keep interior entry 16-byte aligned (matches default HotSpot interior entry alignment).
  return 16;
}

int JeandleAssembler::emit_exception_handler() {
  address base = __ start_a_stub(NativeJump::instruction_size);
  JEANDLE_ERROR_ASSERT_AND_RET_ON_FAIL(base != nullptr, "exception handler stub overflow", 0);
  int offset = __ offset();
  __ jump(RuntimeAddress(JeandleRuntimeRoutine::get_routine_entry(JeandleRuntimeRoutine::_exception_handler)));
  assert(__ offset() - offset <= (int)NativeJump::instruction_size, "overflow");
  __ end_a_stub();
  return offset;
}

using LinkKind_x86_64 = llvm::jitlink::x86_64::EdgeKind_x86_64;

void JeandleAssembler::emit_section_word_reloc(int operand_offset, LinkKind kind, int64_t addend, address target, int reloc_section) {
  assert(operand_offset >= 0, "invalid operand address");
  assert(kind == LinkKind_x86_64::Delta32, "invalid link kind");

  if (reloc_section == CodeBuffer::SECT_INSTS) {
    address at_address = __ code()->insts_begin() + operand_offset;

    // Get the gap size from at_address to the next instrcution. Because it is calculate into addend
    // and we need to recover it to get the correct reloc_target.
    JeandleInstructionResolver* resolver = new JeandleInstructionResolver(__ code()->insts_begin(),
                                                                          __ code()->insts_size(),
                                                                          llvm::Triple(llvm::sys::getProcessTriple()));
    address next_instrction = resolver->lookup_next_instruction(at_address);

    address reloc_target = target + addend + (next_instrction - at_address);
    RelocationHolder rspec = jeandle_section_word_Relocation::spec(reloc_target, CodeBuffer::SECT_CONSTS);

    __ code()->insts()->relocate(at_address, rspec, __ disp32_operand);
  } else {
    assert(reloc_section == CodeBuffer::SECT_CONSTS, "unexpected code section");
    address at_address = __ code()->consts()->start() + operand_offset;
    address reloc_target = target + addend + sizeof(int32_t);
    RelocationHolder rspec = jeandle_section_word_Relocation::spec(reloc_target, CodeBuffer::SECT_INSTS);

    __ code()->consts()->relocate(at_address, rspec, __ disp32_operand);
  }
}

void JeandleAssembler::emit_oop_reloc(int offset, jobject oop_handle) {
  int index = __ oop_recorder()->find_index(oop_handle);
  RelocationHolder rspec = jeandle_oop_Relocation::spec(index);
  address at_address = __ code()->insts_begin() + offset;
  __ code_section()->relocate(at_address, rspec, __ disp32_operand);
}

int JeandleAssembler::fixup_call_inst_offset(int offset) {
  assert(offset >= 0, "invalid offset");
  return offset - NativeJump::data_offset + NativeJump::instruction_size;
}

bool JeandleAssembler::is_oop_reloc(LinkSymbol& target, LinkKind kind) {
  return !target.isDefined() && kind == LinkKind_x86_64::Delta32;
}

bool JeandleAssembler::is_routine_call_reloc(LinkSymbol& target, LinkKind kind) {
  llvm::StringRef target_name = target.hasName() ? *(target.getName()) : "";
  return !target_name.empty() && !target.isDefined() &&
         JeandleRuntimeRoutine::is_routine_entry(target_name) &&
         kind == LinkKind_x86_64::BranchPCRel32;
}

bool JeandleAssembler::is_external_call_reloc(LinkSymbol& target, LinkKind kind) {
  llvm::StringRef target_name = target.hasName() ? *(target.getName()) : "";
  return !target_name.empty() && !target.isDefined() &&
         !JeandleRuntimeRoutine::is_routine_entry(target_name) &&
         kind == LinkKind_x86_64::BranchPCRel32;
}

bool JeandleAssembler::is_const_reloc(LinkSymbol& target, LinkKind kind) {
  return target.isDefined() && kind == LinkKind_x86_64::Delta32;
}
