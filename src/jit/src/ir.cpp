#include <brimir/jit/ir.hpp>

#include <cstdio>
#include <iterator>

namespace brimir::jit {

namespace {

struct OpInfo {
    const char *name;
    bool hasDst;
    uint8_t numSrcs;
    bool usesSize;
    bool isExit; // must be, and may only be, the last instruction
};

constexpr OpInfo kOpInfo[] = {
    {"Const", true, 0, false, false},          {"GetReg", true, 0, false, false},
    {"SetReg", false, 1, false, false},        {"GetPR", true, 0, false, false},
    {"GetT", true, 0, false, false},           {"SetT", false, 1, false, false},
    {"Add", true, 2, false, false},            {"Sub", true, 2, false, false},
    {"CmpEq", true, 2, false, false},          {"SExt8", true, 1, false, false},
    {"SExt16", true, 1, false, false},         {"Load", true, 1, true, false},
    {"Store", false, 2, true, false},          {"AddCycles", false, 0, false, false},
    {"AddAccessCycles", false, 1, true, false}, {"WbStall", false, 0, false, false},
    {"SetWb", false, 0, false, false},         {"SyncCycles", false, 0, false, false},
    {"CheckBoundary", false, 0, false, false}, {"Refill", false, 0, false, false},
    {"SetupDelaySlot", false, 1, false, false}, {"EndDelaySlot", false, 0, false, false},
    {"ExitIfBusWait", false, 1, true, false},  {"ExitIf", false, 1, false, false},
    {"Exit", false, 0, false, true},           {"ExitDynamic", false, 0, false, true},
};
static_assert(std::size(kOpInfo) == static_cast<size_t>(Op::ExitDynamic) + 1, "kOpInfo must cover every Op");

const OpInfo &Info(Op op) {
    return kOpInfo[static_cast<size_t>(op)];
}

} // namespace

ValueId Builder::NewValue() {
    return m_block.numValues++;
}

Inst &Builder::Emit(Op op) {
    Inst &inst = m_block.code.emplace_back();
    inst.op = op;
    return inst;
}

ValueId Builder::Const(uint32_t value) {
    Inst &inst = Emit(Op::Const);
    inst.imm = value;
    return inst.dst = NewValue();
}

ValueId Builder::GetReg(uint32_t reg) {
    Inst &inst = Emit(Op::GetReg);
    inst.imm = reg;
    return inst.dst = NewValue();
}

void Builder::SetReg(uint32_t reg, ValueId value) {
    Inst &inst = Emit(Op::SetReg);
    inst.imm = reg;
    inst.a = value;
}

ValueId Builder::GetPR() {
    return Emit(Op::GetPR).dst = NewValue();
}

ValueId Builder::GetT() {
    return Emit(Op::GetT).dst = NewValue();
}

void Builder::SetT(ValueId value) {
    Emit(Op::SetT).a = value;
}

ValueId Builder::Add(ValueId a, ValueId b) {
    Inst &inst = Emit(Op::Add);
    inst.a = a;
    inst.b = b;
    return inst.dst = NewValue();
}

ValueId Builder::Sub(ValueId a, ValueId b) {
    Inst &inst = Emit(Op::Sub);
    inst.a = a;
    inst.b = b;
    return inst.dst = NewValue();
}

ValueId Builder::CmpEq(ValueId a, ValueId b) {
    Inst &inst = Emit(Op::CmpEq);
    inst.a = a;
    inst.b = b;
    return inst.dst = NewValue();
}

ValueId Builder::SExt8(ValueId a) {
    Inst &inst = Emit(Op::SExt8);
    inst.a = a;
    return inst.dst = NewValue();
}

ValueId Builder::SExt16(ValueId a) {
    Inst &inst = Emit(Op::SExt16);
    inst.a = a;
    return inst.dst = NewValue();
}

ValueId Builder::Load(ValueId address, uint8_t size, bool instrFetch) {
    Inst &inst = Emit(Op::Load);
    inst.a = address;
    inst.size = size;
    inst.flag = instrFetch;
    return inst.dst = NewValue();
}

void Builder::Store(ValueId address, uint8_t size, ValueId value) {
    Inst &inst = Emit(Op::Store);
    inst.a = address;
    inst.b = value;
    inst.size = size;
}

void Builder::AddCycles(uint32_t cycles) {
    Emit(Op::AddCycles).imm = cycles;
}

void Builder::AddAccessCycles(ValueId address, uint8_t size, bool write) {
    Inst &inst = Emit(Op::AddAccessCycles);
    inst.a = address;
    inst.size = size;
    inst.flag = write;
}

void Builder::WbStall(uint32_t mask) {
    Emit(Op::WbStall).imm = mask;
}

void Builder::SetWb(uint8_t reg) {
    Emit(Op::SetWb).imm = reg;
}

void Builder::SyncCycles() {
    Emit(Op::SyncCycles);
}

void Builder::CheckBoundary(uint32_t pc, uint8_t retired) {
    Inst &inst = Emit(Op::CheckBoundary);
    inst.imm = pc;
    inst.retired = retired;
}

void Builder::Refill(uint32_t address) {
    Emit(Op::Refill).imm = address;
}

void Builder::SetupDelaySlot(ValueId target) {
    Emit(Op::SetupDelaySlot).a = target;
}

void Builder::EndDelaySlot() {
    Emit(Op::EndDelaySlot);
}

void Builder::ExitIfBusWait(ValueId address, uint8_t size, bool write, uint32_t pc, uint8_t retired) {
    Inst &inst = Emit(Op::ExitIfBusWait);
    inst.a = address;
    inst.size = size;
    inst.flag = write;
    inst.imm = pc;
    inst.retired = retired;
}

void Builder::ExitIf(ValueId cond, uint32_t pc, uint32_t takenCycles, bool refill, uint8_t retired) {
    Inst &inst = Emit(Op::ExitIf);
    inst.a = cond;
    inst.imm = pc;
    inst.imm2 = takenCycles;
    inst.flag = refill;
    inst.retired = retired;
}

void Builder::Exit(uint32_t pc, uint8_t retired) {
    Inst &inst = Emit(Op::Exit);
    inst.imm = pc;
    inst.retired = retired;
}

void Builder::ExitDynamic(uint8_t retired) {
    Emit(Op::ExitDynamic).retired = retired;
}

const char *OpName(Op op) {
    return static_cast<size_t>(op) < std::size(kOpInfo) ? Info(op).name : "<invalid>";
}

std::string VerifyBlock(const Block &block) {
    if (block.guestInstrCount == 0) {
        return block.code.empty() ? std::string{} : std::string{"empty block must have no code"};
    }
    if (block.code.empty()) {
        return "block has no code";
    }
    if (block.numValues > kMaxValues) {
        return "too many values";
    }

    std::vector<bool> defined(block.numValues, false);
    for (size_t i = 0; i < block.code.size(); ++i) {
        const Inst &inst = block.code[i];
        const auto error = [&](const char *message) {
            return "inst " + std::to_string(i) + " (" + OpName(inst.op) + "): " + message;
        };
        if (static_cast<size_t>(inst.op) >= std::size(kOpInfo)) {
            return error("invalid op");
        }
        const OpInfo &info = Info(inst.op);

        const ValueId srcs[2] = {inst.a, inst.b};
        for (uint8_t s = 0; s < info.numSrcs; ++s) {
            if (srcs[s] >= block.numValues || !defined[srcs[s]]) {
                return error("uses an undefined value");
            }
        }
        if (info.hasDst) {
            if (inst.dst >= block.numValues) {
                return error("destination out of range");
            }
            if (defined[inst.dst]) {
                return error("value defined twice");
            }
            defined[inst.dst] = true;
        }
        if (info.usesSize && inst.size != 1 && inst.size != 2 && inst.size != 4) {
            return error("invalid access size");
        }
        if ((inst.op == Op::GetReg || inst.op == Op::SetReg) && inst.imm > 15) {
            return error("register index out of range");
        }
        const bool last = i + 1 == block.code.size();
        if (info.isExit && !last) {
            return error("exit before end of block");
        }
        if (!info.isExit && last) {
            return error("block does not end with an exit");
        }
    }
    return {};
}

std::string PrintBlock(const Block &block) {
    std::string out;
    char line[192];
    std::snprintf(line, sizeof(line), "block @%08X: %u guest instructions, %u values\n", block.startPC,
                  block.guestInstrCount, static_cast<unsigned>(block.numValues));
    out += line;
    for (const Inst &inst : block.code) {
        if (static_cast<size_t>(inst.op) >= std::size(kOpInfo)) {
            out += "  <invalid op>\n";
            continue;
        }
        const OpInfo &info = Info(inst.op);
        std::string text = "  ";
        if (info.hasDst) {
            text += "v" + std::to_string(inst.dst) + " = ";
        }
        text += info.name;
        if (info.usesSize) {
            text += "." + std::to_string(inst.size * 8);
        }
        if (info.numSrcs >= 1) {
            text += " v" + std::to_string(inst.a);
        }
        if (info.numSrcs >= 2) {
            text += ", v" + std::to_string(inst.b);
        }
        std::snprintf(line, sizeof(line), "  imm=0x%X imm2=%u flag=%d retired=%u\n", inst.imm, inst.imm2,
                      inst.flag ? 1 : 0, static_cast<unsigned>(inst.retired));
        out += text + line;
    }
    return out;
}

} // namespace brimir::jit
