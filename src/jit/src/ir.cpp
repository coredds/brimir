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
    {"SExt16", true, 1, false, false},         {"And", true, 2, false, false},
    {"Or", true, 2, false, false},             {"Xor", true, 2, false, false},
    {"Not", true, 1, false, false},            {"Shl", true, 1, false, false},
    {"Shr", true, 1, false, false},            {"Sar", true, 1, false, false},
    {"CmpGtU", true, 2, false, false},         {"CmpGeU", true, 2, false, false},
    {"CmpGtS", true, 2, false, false},         {"CmpGeS", true, 2, false, false},
    {"GetGBR", true, 0, false, false},         {"SetGBR", false, 1, false, false},
    {"GetVBR", true, 0, false, false},         {"SetVBR", false, 1, false, false},
    {"SetPR", false, 1, false, false},         {"GetSR", true, 0, false, false},
    {"SetSR", false, 1, false, false},         {"GetMACH", true, 0, false, false},
    {"GetMACL", true, 0, false, false},        {"SetMACH", false, 1, false, false},
    {"SetMACL", false, 1, false, false},       {"ClearIntrAllow", false, 0, false, false},
    {"SetIntrAllow", false, 0, false, false},  {"GetDelayTarget", true, 0, false, false},
    {"Mul", true, 2, false, false},            {"MulHiS", true, 2, false, false},
    {"MulHiU", true, 2, false, false},         {"SetSRBits", false, 1, false, false},
    {"Div1", true, 2, false, false},           {"MacW", false, 2, false, false},
    {"MacL", false, 2, false, false},          {"AddAccessCyclesRMWByte", false, 1, false, false},
    {"Load", true, 1, true, false},
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

ValueId Builder::Binary(Op op, ValueId a, ValueId b) {
    Inst &inst = Emit(op);
    inst.a = a;
    inst.b = b;
    return inst.dst = NewValue();
}

ValueId Builder::Unary(Op op, ValueId a, uint32_t imm) {
    Inst &inst = Emit(op);
    inst.a = a;
    inst.imm = imm;
    return inst.dst = NewValue();
}

ValueId Builder::Nullary(Op op) {
    return Emit(op).dst = NewValue();
}

void Builder::Sink(Op op, ValueId a) {
    Emit(op).a = a;
}

ValueId Builder::And(ValueId a, ValueId b) {
    return Binary(Op::And, a, b);
}

ValueId Builder::Or(ValueId a, ValueId b) {
    return Binary(Op::Or, a, b);
}

ValueId Builder::Xor(ValueId a, ValueId b) {
    return Binary(Op::Xor, a, b);
}

ValueId Builder::Not(ValueId a) {
    return Unary(Op::Not, a);
}

ValueId Builder::Shl(ValueId a, uint32_t amount) {
    return Unary(Op::Shl, a, amount);
}

ValueId Builder::Shr(ValueId a, uint32_t amount) {
    return Unary(Op::Shr, a, amount);
}

ValueId Builder::Sar(ValueId a, uint32_t amount) {
    return Unary(Op::Sar, a, amount);
}

ValueId Builder::CmpGtU(ValueId a, ValueId b) {
    return Binary(Op::CmpGtU, a, b);
}

ValueId Builder::CmpGeU(ValueId a, ValueId b) {
    return Binary(Op::CmpGeU, a, b);
}

ValueId Builder::CmpGtS(ValueId a, ValueId b) {
    return Binary(Op::CmpGtS, a, b);
}

ValueId Builder::CmpGeS(ValueId a, ValueId b) {
    return Binary(Op::CmpGeS, a, b);
}

ValueId Builder::GetGBR() {
    return Nullary(Op::GetGBR);
}

void Builder::SetGBR(ValueId value) {
    Sink(Op::SetGBR, value);
}

ValueId Builder::GetVBR() {
    return Nullary(Op::GetVBR);
}

void Builder::SetVBR(ValueId value) {
    Sink(Op::SetVBR, value);
}

void Builder::SetPR(ValueId value) {
    Sink(Op::SetPR, value);
}

ValueId Builder::GetSR() {
    return Nullary(Op::GetSR);
}

void Builder::SetSR(ValueId value, bool delaySlot) {
    Inst &inst = Emit(Op::SetSR);
    inst.a = value;
    inst.flag = delaySlot;
}

ValueId Builder::GetMACH() {
    return Nullary(Op::GetMACH);
}

ValueId Builder::GetMACL() {
    return Nullary(Op::GetMACL);
}

void Builder::SetMACH(ValueId value) {
    Sink(Op::SetMACH, value);
}

void Builder::SetMACL(ValueId value) {
    Sink(Op::SetMACL, value);
}

void Builder::ClearIntrAllow() {
    Emit(Op::ClearIntrAllow);
}

void Builder::SetIntrAllow() {
    Emit(Op::SetIntrAllow);
}

ValueId Builder::GetDelayTarget() {
    return Nullary(Op::GetDelayTarget);
}

ValueId Builder::Mul(ValueId a, ValueId b) {
    return Binary(Op::Mul, a, b);
}

ValueId Builder::MulHiS(ValueId a, ValueId b) {
    return Binary(Op::MulHiS, a, b);
}

ValueId Builder::MulHiU(ValueId a, ValueId b) {
    return Binary(Op::MulHiU, a, b);
}

void Builder::SetSRBits(ValueId value, uint32_t mask) {
    Inst &inst = Emit(Op::SetSRBits);
    inst.a = value;
    inst.imm = mask;
}

ValueId Builder::Div1(ValueId rn, ValueId rm, bool rmIsRn) {
    Inst &inst = Emit(Op::Div1);
    inst.a = rn;
    inst.b = rm;
    inst.flag = rmIsRn;
    return inst.dst = NewValue();
}

void Builder::MacW(ValueId op1, ValueId op2) {
    Inst &inst = Emit(Op::MacW);
    inst.a = op1;
    inst.b = op2;
}

void Builder::MacL(ValueId op1, ValueId op2) {
    Inst &inst = Emit(Op::MacL);
    inst.a = op1;
    inst.b = op2;
}

void Builder::AddAccessCyclesRMWByte(ValueId address) {
    Sink(Op::AddAccessCyclesRMWByte, address);
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

void Builder::KnownRefill(uint32_t address, uint32_t value) {
    Inst &inst = Emit(Op::Refill);
    inst.imm = address;
    inst.imm2 = value;
    inst.flag = true;
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
    // guestOpcodes is optional for hand-built blocks (empty: nothing is checked on entry), but when
    // present it holds exactly the instruction words plus the tail word.
    if (!block.guestOpcodes.empty() &&
        block.guestOpcodes.size() != block.guestInstrCount + (block.hasTailWord ? 1u : 0u)) {
        return "guestOpcodes does not match guestInstrCount and hasTailWord";
    }
    if (block.hasTailWord && block.guestOpcodes.empty()) {
        return "tail word without guestOpcodes";
    }
    if (block.fetchFromArrays && block.guestOpcodes.empty()) {
        return "fetchFromArrays without guestOpcodes";
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
        if ((inst.op == Op::Shl || inst.op == Op::Shr || inst.op == Op::Sar) && (inst.imm < 1 || inst.imm > 31)) {
            return error("shift amount out of range");
        }
        if (inst.op == Op::SetSRBits && (inst.imm & ~0x303u) != 0) {
            // Only T/S/Q/M: ILevel changes need the setSR callback (interrupt recompute)
            return error("SetSRBits mask outside T/S/Q/M");
        }
        if (inst.op == Op::Refill && inst.flag) {
            // A known refill reads two words of guestOpcodes, and imm2 must be exactly those words.
            const uint32_t offset = inst.imm - block.startPC;
            const size_t words = block.guestOpcodes.size();
            if ((inst.imm & 3u) != 0 || inst.imm < block.startPC || (offset & 1u) != 0 || offset / 2 + 1 >= words) {
                return error("known refill outside guestOpcodes");
            }
            const uint32_t expected = (static_cast<uint32_t>(block.guestOpcodes[offset / 2]) << 16) |
                                      block.guestOpcodes[offset / 2 + 1];
            if (inst.imm2 != expected) {
                return error("known refill value differs from guestOpcodes");
            }
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
