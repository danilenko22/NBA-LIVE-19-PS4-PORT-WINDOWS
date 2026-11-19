// SPDX-FileCopyrightText: Copyright 2026 NBA LIVE 19 native port
// SPDX-License-Identifier: GPL-2.0-or-later

// NBA LIVE 19 (basket nets): a pixel shader alpha-tests (alpha > 0.2) and only then reads its
// quad neighbours' interpolated positions (ds_swizzle / DPP quad perm -> QuadBroadcast) to build a
// face normal from their differences. On GCN the alpha test only narrows EXEC: the lanes that
// failed it still hold their interpolated attributes in VGPRs, and the swizzle reads them. In
// SPIR-V the broadcast sits inside the structured branch, where the failed lanes are inactive, so
// the value read from them is undefined: at strand edges (quads mixing kept and killed pixels)
// the normal became NaN, and the lit colour too (black dots that flickered with the jitter).
//
// A QuadBroadcast whose value is an interpolated attribute (with bit casts) is the same wherever
// it is computed, so it is moved, with that chain, to the end of the entry block, where every lane
// of the quad is still active.

#include <cstdlib>
#include <unordered_map>
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

static bool IsInvariantChain(const IR::Value& value, int depth = 0) {
    if (value.IsImmediate()) {
        return true;
    }
    if (depth > 8) {
        return false;
    }
    const IR::Inst* inst = value.Inst();
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetAttribute:
    case IR::Opcode::GetAttributeU32:
    case IR::Opcode::BitCastF32U32:
    case IR::Opcode::BitCastU32F32:
        break;
    default:
        return false;
    }
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        if (!IsInvariantChain(inst->Arg(i), depth + 1)) {
            return false;
        }
    }
    return true;
}

static IR::Value CloneInto(IR::Block* entry, const IR::Value& value,
                           std::unordered_map<const IR::Inst*, IR::Inst*>& clones) {
    if (value.IsImmediate()) {
        return value;
    }
    const IR::Inst* inst = value.Inst();
    if (const auto it = clones.find(inst); it != clones.end()) {
        return IR::Value{it->second};
    }
    IR::Value args[3]{};
    const size_t num_args = inst->NumArgs();
    for (size_t i = 0; i < num_args; ++i) {
        args[i] = CloneInto(entry, inst->Arg(i), clones);
    }
    const auto op = inst->GetOpcode();
    const u32 flags = inst->Flags<u32>();
    IR::Block::iterator it;
    switch (num_args) {
    case 0:
        it = entry->PrependNewInst(entry->end(), op, {}, flags);
        break;
    case 1:
        it = entry->PrependNewInst(entry->end(), op, {args[0]}, flags);
        break;
    case 2:
        it = entry->PrependNewInst(entry->end(), op, {args[0], args[1]}, flags);
        break;
    default:
        it = entry->PrependNewInst(entry->end(), op, {args[0], args[1], args[2]}, flags);
        break;
    }
    clones.emplace(inst, &*it);
    return IR::Value{&*it};
}

void HoistQuadBroadcastPass(IR::Program& program) {
    static const bool enabled = [] {
        const char* e = std::getenv("BB_NBA_HOIST_QUAD");
        return !e || e[0] != '0';
    }();
    if (!enabled || program.info.sw_stage != SwStage::Fragment || program.syntax_list.empty()) {
        return;
    }
    IR::Block* const entry = program.syntax_list.front().data.block;
    std::unordered_map<const IR::Inst*, IR::Inst*> clones;
    for (IR::Block* const block : program.blocks) {
        if (block == entry) {
            continue;
        }
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::QuadBroadcast || !inst.Arg(1).IsImmediate() ||
                !IsInvariantChain(inst.Arg(0))) {
                continue;
            }
            const IR::Value source = CloneInto(entry, inst.Arg(0), clones);
            const auto it = entry->PrependNewInst(entry->end(), IR::Opcode::QuadBroadcast,
                                                  {source, inst.Arg(1)});
            inst.ReplaceUsesWith(IR::Value{&*it});
        }
    }
}

} // namespace Shader::Optimization
