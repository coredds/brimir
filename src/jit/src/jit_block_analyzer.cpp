#include "../include/jit_block_analyzer.hpp"
#include <cassert>

namespace brimir::jit {

BlockAnalyzer::BlockAnalyzer(MemoryReadFn read_fn)
    : read_memory_(std::move(read_fn)) 
{
    assert(read_memory_ && "Memory read function must be provided");
}

std::unique_ptr<IRBlock> BlockAnalyzer::Analyze(uint32 start_pc, size_t max_instructions) {
    auto block = std::make_unique<IRBlock>(start_pc);
    
    uint32 pc = start_pc;
    size_t instr_count = 0;
    bool block_ended = false;
    
    while (!block_ended && instr_count < max_instructions) {
        // Fetch instruction
        uint16 raw_instr;
        if (!FetchAndDecode(pc, raw_instr)) {
            // Failed to fetch, end block
            break;
        }
        
        // Translate to IR
        if (!TranslateToIR(pc, raw_instr, *block)) {
            // Failed to translate, end block
            break;
        }
        
        // Check if this ends the block
        if (IsBlockTerminator(raw_instr)) {
            block_ended = true;
        }
        
        pc += 2; // SH-2 instructions are 2 bytes
        instr_count++;
    }
    
    // Ensure block has at least one instruction
    if (block->Empty()) {
        return nullptr;
    }
    
    return block;
}

bool BlockAnalyzer::IsBlockStart(uint32 addr) const {
    // Blocks start at:
    // 1. Even addresses (SH-2 requirement)
    // 2. After branches
    // 3. Entry points
    return (addr & 1) == 0;
}

LiveRanges BlockAnalyzer::AnalyzeLiveness(const IRBlock& block) const {
    LiveRanges ranges;
    ranges.per_instr.resize(block.Size(), 0);
    
    // Simple backward dataflow analysis
    uint16 live = 0;
    
    for (ssize_t i = block.Size() - 1; i >= 0; i--) {
        const auto& instr = block.instructions[i];
        
        // Mark sources as live
        if (instr.src1.type == IROperandType::REG) {
            live |= (1 << instr.src1.reg);
        }
        if (instr.src2.type == IROperandType::REG) {
            live |= (1 << instr.src2.reg);
        }
        
        ranges.per_instr[i] = live;
        
        // Destination kills liveness (if not also a source)
        if (instr.dst.type == IROperandType::REG) {
            live &= ~(1 << instr.dst.reg);
        }
    }
    
    ranges.live_in = live;
    ranges.live_out = (block.Size() > 0) ? ranges.per_instr[block.Size() - 1] : 0;
    
    return ranges;
}

bool BlockAnalyzer::FetchAndDecode(uint32 addr, uint16& raw_instr) {
    try {
        raw_instr = read_memory_(addr);
        return true;
    } catch (...) {
        return false;
    }
}

bool BlockAnalyzer::TranslateToIR(uint32 addr, uint16 instr, IRBlock& block) {
    // Decode instruction based on Saturn Open SDK patterns
    // Format: hhhhnnnnmmmmllll where h=high nibble, n=Rn, m=Rm, l=low nibble
    
    uint8 high_nibble = (instr >> 12) & 0xF;
    uint8 rn = GetRn(instr);
    uint8 rm = GetRm(instr);
    uint8 low_nibble = instr & 0xF;
    
    // Decode based on high nibble (primary dispatch)
    switch (high_nibble) {
        case 0x0:
            // 0000 group - various operations
            if (instr == 0x0009) {
                // NOP
                block.Add(IRInstruction(IROp::NOP, IROperand(), IROperand(), IROperand(), 1, addr));
                return true;
            }
            break;
            
        case 0x1:
            // 0001nnnnmmmmllll - MOV.L Rm, @(disp, Rn)
            // Store operations
            break;
            
        case 0x2:
            // 0010 group - memory operations
            if (low_nibble == 0x0) {
                // MOV.B Rm, @Rn - Store byte
                block.Add(IRInstruction(IROp::STORE8, 
                    IROperand::Reg(rn), IROperand::Reg(rm), 
                    IROperand(), 1, addr));
                return true;
            }
            break;
            
        case 0x3:
            // 0011 group - arithmetic
            if (low_nibble == 0xC) {
                // ADD Rm, Rn
                block.Add(IRInstruction(IROp::ADD,
                    IROperand::Reg(rn), IROperand::Reg(rn), IROperand::Reg(rm),
                    1, addr));
                return true;
            }
            break;
            
        case 0x6:
            // 0110 group - register moves
            if (low_nibble == 0x3) {
                // MOV Rm, Rn
                block.Add(IRInstruction(IROp::MOV_REG,
                    IROperand::Reg(rn), IROperand::Reg(rm),
                    IROperand(), 1, addr));
                return true;
            }
            break;
            
        case 0x7:
            // 0111nnnniiiiiiii - ADD #imm, Rn
            block.Add(IRInstruction(IROp::ADDI,
                IROperand::Reg(rn), IROperand::Reg(rn), IROperand::Imm(GetImm8(instr)),
                1, addr));
            return true;
            
        case 0x8:
            // 1000 group - conditional branches and byte operations
            if (((instr >> 8) & 0xF) == 0xB) {
                // BF/BT - Conditional branch
                sint32 disp = GetImm8(instr);
                uint32 target = addr + 4 + (disp * 2);
                
                block.Add(IRInstruction(IROp::BRANCH_COND,
                    IROperand::Addr(target), IROperand::Flag(0), // T-bit
                    IROperand(), 3, addr)); // Branch with delay slot = 3 cycles
                
                block.exit_type = IRBlock::ExitType::CONDITIONAL;
                block.branch_target = target;
                block.has_delay_slot = true;
                return true;
            }
            break;
            
        case 0x9:
            // 1001nnnniiiiiiii - MOV.W @(disp, PC), Rn
            break;
            
        case 0xA:
            // 1010iiiiiiiiiiii - BRA disp
            {
                sint32 disp = GetDisp12(instr);
                uint32 target = addr + 4 + (disp * 2);
                
                block.Add(IRInstruction(IROp::BRANCH,
                    IROperand::Addr(target), IROperand(), IROperand(),
                    2, addr));
                
                block.exit_type = IRBlock::ExitType::BRANCH;
                block.branch_target = target;
                block.has_delay_slot = true;
                return true;
            }
            
        case 0xB:
            // 1011iiiiiiiiiiii - BSR disp - Branch to subroutine
            {
                sint32 disp = GetDisp12(instr);
                uint32 target = addr + 4 + (disp * 2);
                
                block.Add(IRInstruction(IROp::CALL,
                    IROperand::Addr(target), IROperand(), IROperand(),
                    2, addr));
                
                block.exit_type = IRBlock::ExitType::BRANCH;
                block.branch_target = target;
                block.has_delay_slot = true;
                return true;
            }
            
        case 0xE:
            // 1110nnnniiiiiiii - MOV #imm, Rn
            block.Add(IRInstruction(IROp::MOV_IMM,
                IROperand::Reg(rn), IROperand::Imm(GetImm8(instr)),
                IROperand(), 1, addr));
            return true;
    }
    
    // TODO: Implement all SH-2 instructions from Saturn Open SDK
    // For now, add as NOP and continue
    block.Add(IRInstruction(IROp::NOP, IROperand(), IROperand(), IROperand(), 1, addr));
    return true;
}

bool BlockAnalyzer::IsBlockTerminator(uint16 instr) const {
    // Instructions that end a basic block:
    // - Branches (BRA, BT, BF, BT/S, BF/S)
    // - Jumps (JMP, JSR)
    // - Returns (RTS, RTE)
    // - Traps (TRAPA)
    
    uint8 high_nibble = (instr >> 12) & 0xF;
    
    switch (high_nibble) {
        case 0x0:
            // RTS, RTE, etc.
            if (instr == 0x000B) return true; // RTS
            if (instr == 0x002B) return true; // RTE
            break;
            
        case 0x4:
            // JMP, JSR
            if ((instr & 0xF0FF) == 0x402B) return true; // JMP @Rm
            if ((instr & 0xF0FF) == 0x400B) return true; // JSR @Rm
            break;
            
        case 0x8:
            // Conditional branches BT, BF
            if (((instr >> 8) & 0xF) == 0x9) return true; // BT
            if (((instr >> 8) & 0xF) == 0xB) return true; // BF
            if (((instr >> 8) & 0xF) == 0xD) return true; // BT/S
            if (((instr >> 8) & 0xF) == 0xF) return true; // BF/S
            break;
            
        case 0xA:
            // BRA - Unconditional branch
            return true;
            
        case 0xB:
            // BSR - Branch to subroutine
            return true;
            
        case 0xC:
            // TRAPA
            if (((instr >> 8) & 0xF) == 0x3) return true;
            break;
    }
    
    return false;
}

// BlockCache implementation

IRBlock* BlockCache::Lookup(uint32 addr) {
    auto it = blocks_.find(addr);
    if (it != blocks_.end()) {
        stats_.hits++;
        return it->second.get();
    }
    stats_.misses++;
    return nullptr;
}

void BlockCache::Insert(std::unique_ptr<IRBlock> block) {
    uint32 addr = block->start_addr;
    stats_.block_count++;
    stats_.total_instructions += block->Size();
    blocks_[addr] = std::move(block);
}

void BlockCache::Invalidate(uint32 start, uint32 end) {
    auto it = blocks_.begin();
    while (it != blocks_.end()) {
        if (it->first >= start && it->first < end) {
            stats_.block_count--;
            stats_.total_instructions -= it->second->Size();
            it = blocks_.erase(it);
        } else {
            ++it;
        }
    }
}

void BlockCache::Clear() {
    blocks_.clear();
    stats_ = Stats{};
}

BlockCache::Stats BlockCache::GetStats() const {
    return stats_;
}

} // namespace brimir::jit

