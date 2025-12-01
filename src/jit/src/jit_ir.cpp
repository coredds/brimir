#include "../include/jit_ir.hpp"
#include <sstream>
#include <iomanip>

namespace brimir::jit {

std::string IRInstruction::ToString() const {
    std::ostringstream oss;
    
    // Opcode name
    const char* op_names[] = {
        "MOV_REG", "MOV_IMM",
        "ADD", "ADDI", "ADDC", "SUB", "SUBC", "NEG",
        "AND", "OR", "XOR", "NOT",
        "SHLL", "SHLR", "SHAR", "ROTL", "ROTR",
        "CMP_EQ", "CMP_GE", "CMP_GT", "CMP_HI", "CMP_HS",
        "LOAD8", "LOAD16", "LOAD32", "STORE8", "STORE16", "STORE32",
        "BRANCH", "BRANCH_COND", "CALL", "RETURN",
        "NOP", "TRAP", "SET_T", "CLR_T",
        "EXIT_BLOCK", "UPDATE_CYCLES"
    };
    
    oss << op_names[static_cast<int>(op)] << " ";
    
    // Operands
    auto format_operand = [](const IROperand& op) -> std::string {
        std::ostringstream s;
        switch (op.type) {
            case IROperandType::NONE: return "-";
            case IROperandType::REG: 
                s << "R" << (int)op.reg;
                return s.str();
            case IROperandType::IMM:
                s << "#" << op.imm;
                return s.str();
            case IROperandType::ADDR:
                s << "0x" << std::hex << std::setw(8) << std::setfill('0') << op.addr;
                return s.str();
            case IROperandType::SR_FLAG:
                s << "SR." << (int)op.flag;
                return s.str();
        }
        return "?";
    };
    
    oss << format_operand(dst);
    if (src1.type != IROperandType::NONE) {
        oss << ", " << format_operand(src1);
    }
    if (src2.type != IROperandType::NONE) {
        oss << ", " << format_operand(src2);
    }
    
    oss << " ; " << (int)cycles << " cycles";
    
    return oss.str();
}

std::string IRBlock::ToString() const {
    std::ostringstream oss;
    
    oss << "Block 0x" << std::hex << std::setw(8) << std::setfill('0') << start_addr << ":\n";
    oss << "  Size: " << std::dec << instructions.size() << " instructions\n";
    oss << "  Cycles: " << total_cycles << "\n";
    oss << "  Exit: ";
    
    switch (exit_type) {
        case ExitType::SEQUENTIAL: oss << "Sequential"; break;
        case ExitType::BRANCH: oss << "Branch to 0x" << std::hex << branch_target; break;
        case ExitType::CONDITIONAL: oss << "Conditional"; break;
        case ExitType::DYNAMIC: oss << "Dynamic"; break;
        case ExitType::RETURN: oss << "Return"; break;
    }
    
    oss << "\n\nInstructions:\n";
    for (size_t i = 0; i < instructions.size(); i++) {
        oss << "  " << std::setw(3) << i << ": ";
        oss << instructions[i].ToString() << "\n";
    }
    
    return oss.str();
}

} // namespace brimir::jit

