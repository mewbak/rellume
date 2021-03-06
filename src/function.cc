/**
 * This file is part of Rellume.
 *
 * (c) 2016-2019, Alexis Engelke <alexis.engelke@googlemail.com>
 *
 * Rellume is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License (LGPL)
 * as published by the Free Software Foundation, either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * Rellume is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Rellume.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 **/

#include "function.h"

#include "basicblock.h"
#include "callconv.h"
#include "config.h"
#include "lifter.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <cassert>
#include <cstdint>
#include <memory>
#include <unordered_map>


/**
 * \defgroup LLFunc Func2
 * \brief Representation of a function
 *
 * @{
 **/

namespace rellume {

Function::Function(llvm::Module* mod, LLConfig* cfg) : cfg(cfg)
{
    llvm::LLVMContext& ctx = mod->getContext();
    llvm = llvm::Function::Create(cfg->callconv.FnType(ctx),
                                  llvm::GlobalValue::ExternalLinkage, "", mod);
    llvm->setCallingConv(cfg->callconv.FnCallConv());

    // CPU struct pointer parameters has some extra properties.
    unsigned cpu_param_idx = cfg->callconv.CpuStructParamIdx();
    llvm->addFnAttr("null-pointer-is-valid", "true");
    llvm->addParamAttr(cpu_param_idx, llvm::Attribute::NoAlias);
    llvm->addParamAttr(cpu_param_idx, llvm::Attribute::NoCapture);
    llvm->addParamAttr(cpu_param_idx, llvm::Attribute::getWithAlignment(ctx, 16));
    llvm->addDereferenceableParamAttr(cpu_param_idx, 0x190);

    // Create entry basic block as first block in the function.
    entry_block = std::make_unique<ArchBasicBlock>(llvm, *cfg, BasicBlock::ENTRY);
}

Function::~Function() = default;

void Function::AddInst(uint64_t block_addr, const LLInstr& inst)
{
    if (block_map.size() == 0)
        entry_addr = block_addr;
    if (block_map.find(block_addr) == block_map.end())
        block_map[block_addr] = std::make_unique<ArchBasicBlock>(llvm, *cfg);

    Lifter lifter(*cfg, *block_map[block_addr]);
    lifter.Lift(inst);
}

ArchBasicBlock& Function::ResolveAddr(llvm::Value* addr) {
    if (auto const_addr = llvm::dyn_cast<llvm::ConstantInt>(addr)) {
        auto block_it = block_map.find(const_addr->getZExtValue());
        if (block_it != block_map.end())
            return *(block_it->second);
    }
    return *exit_block;
}

llvm::Function* Function::Lift() {
    if (block_map.size() == 0)
        return nullptr;

    exit_block = std::make_unique<ArchBasicBlock>(llvm, *cfg, BasicBlock::EXIT);

    entry_block->BranchTo(*block_map[entry_addr]);

    for (auto it = block_map.begin(); it != block_map.end(); ++it) {
        llvm::Value* next_rip = it->second->NextRip();
        if (auto select = llvm::dyn_cast<llvm::SelectInst>(next_rip)) {
            it->second->BranchTo(select->getCondition(),
                                 ResolveAddr(select->getTrueValue()),
                                 ResolveAddr(select->getFalseValue()));
        } else {
            it->second->BranchTo(ResolveAddr(next_rip));
        }
    }

    // Walk over blocks as long as phi nodes could have been added. We stop when
    // alls phis are filled.
    // TODO: improve walk ordering and efficiency (e.g. by adding predecessors
    // to the set of remaining blocks when something changed)
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& item : block_map)
            changed |= item.second->FillPhis();
        changed |= exit_block->FillPhis();
    }

    exit_block->RemoveUnmodifiedStores(*entry_block);

    if (cfg->verify_ir && llvm::verifyFunction(*(llvm), &llvm::errs()))
        return nullptr;

    return llvm;
}

}

/**
 * @}
 **/
