#include "x64.hpp"

namespace ron::x64 {

// ───────────────────────────── Raw bytes ─────────────────────────────

void Assembler::dword(uint32_t v) {
    for (int i = 0; i < 4; ++i) code.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void Assembler::qword(uint64_t v) {
    for (int i = 0; i < 8; ++i) code.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void Assembler::patch32(uint32_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) code[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// ───────────────────────────── Encoding helpers ─────────────────────────────

// REX prefix: 0100WRXB. `forceForByteRegs` is needed to address spl/bpl/sil/dil
// instead of ah/ch/dh/bh when an 8-bit register operand is 4..7.
void Assembler::rex(bool w, unsigned reg, unsigned base, bool forceForByteRegs, unsigned index) {
    uint8_t r = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((index & 8) ? 2 : 0) | ((base & 8) ? 1 : 0);
    if (r != 0x40 || forceForByteRegs) byte(r);
}

void Assembler::modrmMem(unsigned reg, const Mem& m) {
    if (m.rip) {
        byte(static_cast<uint8_t>(((reg & 7) << 3) | 5));   // mod=00 rm=101: [rip+disp32]
        fixups.push_back({static_cast<uint32_t>(pos()), m.section, m.target});
        dword(0);
        return;
    }
    unsigned base = m.base & 7;
    uint8_t mod;
    if (m.disp == 0 && base != RBP) mod = 0;   // rbp/r13 as base always need a displacement
    else if (m.disp >= -128 && m.disp <= 127) mod = 1;
    else mod = 2;
    if (m.hasIndex()) {
        uint8_t ss = m.scale == 8 ? 3 : m.scale == 4 ? 2 : m.scale == 2 ? 1 : 0;
        byte(static_cast<uint8_t>((mod << 6) | ((reg & 7) << 3) | 4));   // rm=100: SIB follows
        byte(static_cast<uint8_t>((ss << 6) | ((m.index & 7) << 3) | base));
    } else {
        byte(static_cast<uint8_t>((mod << 6) | ((reg & 7) << 3) | base));
        if (base == RSP) byte(0x24);   // SIB: no index, base = rsp/r12
    }
    if (mod == 1) byte(static_cast<uint8_t>(static_cast<int8_t>(m.disp)));
    else if (mod == 2) dword(static_cast<uint32_t>(m.disp));
}

void Assembler::emitRM(uint8_t legacy, bool w, std::initializer_list<uint8_t> opcode, unsigned reg,
                       const Mem& m, bool byteRegs) {
    if (legacy) byte(legacy);
    rex(w, reg, m.rip ? 0 : m.base, byteRegs && (reg & 7) >= 4 && reg < 8, m.hasIndex() ? m.index : 0);
    for (uint8_t b : opcode) byte(b);
    modrmMem(reg, m);
}

void Assembler::emitRR(uint8_t legacy, bool w, std::initializer_list<uint8_t> opcode, unsigned reg,
                       unsigned rm, bool byteRegs) {
    if (legacy) byte(legacy);
    bool force = byteRegs && (((reg & 7) >= 4 && reg < 8) || ((rm & 7) >= 4 && rm < 8));
    rex(w, reg, rm, force);
    for (uint8_t b : opcode) byte(b);
    byte(static_cast<uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

// ───────────────────────────── Labels ─────────────────────────────

Label Assembler::newLabel() {
    labels_.emplace_back();
    return static_cast<Label>(labels_.size() - 1);
}

void Assembler::bind(Label l) {
    LabelInfo& info = labels_[l];
    info.pos = static_cast<int64_t>(pos());
    for (uint32_t ref : info.refs) patch32(ref, static_cast<uint32_t>(info.pos - (ref + 4)));
    info.refs.clear();
}

void Assembler::rel32To(Label l) {
    LabelInfo& info = labels_[l];
    uint32_t at = static_cast<uint32_t>(pos());
    if (info.pos >= 0) {
        dword(static_cast<uint32_t>(info.pos - (at + 4)));
    } else {
        info.refs.push_back(at);
        dword(0);
    }
}

void Assembler::jmp(Label l) {
    byte(0xE9);
    rel32To(l);
}

void Assembler::jcc(Cond c, Label l) {
    byte(0x0F);
    byte(static_cast<uint8_t>(0x80 | c));
    rel32To(l);
}

void Assembler::call(Label l) {
    byte(0xE8);
    rel32To(l);
}

// ───────────────────────────── Integer moves ─────────────────────────────

void Assembler::movRR(Reg dst, Reg src) {
    if (dst != src) emitRR(0, true, {0x89}, src, dst);
}

void Assembler::movRR32(Reg dst, Reg src) { emitRR(0, false, {0x89}, src, dst); }

void Assembler::movRI(Reg dst, uint64_t imm) {
    if (imm == 0) {
        emitRR(0, false, {0x31}, dst, dst);   // xor r32, r32
    } else if (imm <= 0xFFFFFFFFull) {
        rex(false, 0, dst, false);
        byte(static_cast<uint8_t>(0xB8 + (dst & 7)));   // mov r32, imm32 (zero-extends)
        dword(static_cast<uint32_t>(imm));
    } else if (static_cast<int64_t>(imm) >= INT32_MIN && static_cast<int64_t>(imm) < 0) {
        rex(true, 0, dst, false);
        byte(0xC7);   // mov r/m64, imm32 (sign-extends)
        byte(static_cast<uint8_t>(0xC0 | (dst & 7)));
        dword(static_cast<uint32_t>(imm));
    } else {
        rex(true, 0, dst, false);
        byte(static_cast<uint8_t>(0xB8 + (dst & 7)));   // movabs r64, imm64
        qword(imm);
    }
}

void Assembler::load(Reg dst, const Mem& m, unsigned size, bool signExtend) {
    switch (size) {
    case 8: emitRM(0, true, {0x8B}, dst, m); break;
    case 4:
        if (signExtend) emitRM(0, true, {0x63}, dst, m);   // movsxd
        else emitRM(0, false, {0x8B}, dst, m);
        break;
    case 2:
        if (signExtend) emitRM(0, true, {0x0F, 0xBF}, dst, m);
        else emitRM(0, false, {0x0F, 0xB7}, dst, m);
        break;
    default:
        if (signExtend) emitRM(0, true, {0x0F, 0xBE}, dst, m);
        else emitRM(0, false, {0x0F, 0xB6}, dst, m);
        break;
    }
}

void Assembler::store(const Mem& m, Reg src, unsigned size) {
    switch (size) {
    case 8: emitRM(0, true, {0x89}, src, m); break;
    case 4: emitRM(0, false, {0x89}, src, m); break;
    case 2: emitRM(0x66, false, {0x89}, src, m); break;
    default: emitRM(0, false, {0x88}, src, m, true); break;
    }
}

void Assembler::storeImm32(const Mem& m, int32_t imm, unsigned size) {
    // RIP-relative operands must not be followed by an immediate (fixups assume disp32 is last).
    emitRM(0, size == 8, {0xC7}, 0, m);
    dword(static_cast<uint32_t>(imm));
}

void Assembler::lea(Reg dst, const Mem& m) { emitRM(0, true, {0x8D}, dst, m); }

void Assembler::movsx(Reg dst, Reg src, unsigned fromSize) {
    switch (fromSize) {
    case 1: emitRR(0, true, {0x0F, 0xBE}, dst, src, true); break;
    case 2: emitRR(0, true, {0x0F, 0xBF}, dst, src); break;
    case 4: emitRR(0, true, {0x63}, dst, src); break;
    default: movRR(dst, src); break;
    }
}

void Assembler::movzx(Reg dst, Reg src, unsigned fromSize) {
    switch (fromSize) {
    case 1: emitRR(0, false, {0x0F, 0xB6}, dst, src, true); break;
    case 2: emitRR(0, false, {0x0F, 0xB7}, dst, src); break;
    case 4: movRR32(dst, src); break;
    default: movRR(dst, src); break;
    }
}

// ───────────────────────────── Integer arithmetic ─────────────────────────────

void Assembler::add(Reg dst, Reg src) { emitRR(0, true, {0x01}, src, dst); }
void Assembler::sub(Reg dst, Reg src) { emitRR(0, true, {0x29}, src, dst); }
void Assembler::and_(Reg dst, Reg src) { emitRR(0, true, {0x21}, src, dst); }
void Assembler::or_(Reg dst, Reg src) { emitRR(0, true, {0x09}, src, dst); }
void Assembler::xor_(Reg dst, Reg src) { emitRR(0, true, {0x31}, src, dst); }
void Assembler::cmp(Reg a, Reg b) { emitRR(0, true, {0x39}, b, a); }
void Assembler::test(Reg a, Reg b) { emitRR(0, true, {0x85}, b, a); }
void Assembler::imul(Reg dst, Reg src) { emitRR(0, true, {0x0F, 0xAF}, dst, src); }

void Assembler::imulImm(Reg dst, Reg src, int32_t imm) {
    emitRR(0, true, {0x69}, dst, src);
    dword(static_cast<uint32_t>(imm));
}

void Assembler::addImm(Reg dst, int32_t imm) {
    if (imm == 0) return;
    if (imm >= -128 && imm <= 127) {
        emitRR(0, true, {0x83}, 0, dst);
        byte(static_cast<uint8_t>(static_cast<int8_t>(imm)));
    } else {
        emitRR(0, true, {0x81}, 0, dst);
        dword(static_cast<uint32_t>(imm));
    }
}

void Assembler::subImm(Reg dst, int32_t imm) {
    if (imm == 0) return;
    if (imm >= -128 && imm <= 127) {
        emitRR(0, true, {0x83}, 5, dst);
        byte(static_cast<uint8_t>(static_cast<int8_t>(imm)));
    } else {
        emitRR(0, true, {0x81}, 5, dst);
        dword(static_cast<uint32_t>(imm));
    }
}

void Assembler::subImm32Patchable(Reg dst, uint32_t& immOffset) {
    emitRR(0, true, {0x81}, 5, dst);
    immOffset = static_cast<uint32_t>(pos());
    dword(0);
}

void Assembler::cmpImm(Reg a, int32_t imm) {
    if (imm >= -128 && imm <= 127) {
        emitRR(0, true, {0x83}, 7, a);
        byte(static_cast<uint8_t>(static_cast<int8_t>(imm)));
    } else {
        emitRR(0, true, {0x81}, 7, a);
        dword(static_cast<uint32_t>(imm));
    }
}

void Assembler::aluImm(unsigned ext, Reg dst, int32_t imm) {
    if (imm >= -128 && imm <= 127) {
        emitRR(0, true, {0x83}, ext, dst);
        byte(static_cast<uint8_t>(static_cast<int8_t>(imm)));
    } else {
        emitRR(0, true, {0x81}, ext, dst);
        dword(static_cast<uint32_t>(imm));
    }
}

void Assembler::andImm(Reg dst, int32_t imm) { aluImm(4, dst, imm); }
void Assembler::orImm(Reg dst, int32_t imm) { aluImm(1, dst, imm); }
void Assembler::xorImm(Reg dst, int32_t imm) { aluImm(6, dst, imm); }

void Assembler::xorImm8(Reg dst, int8_t imm) {
    emitRR(0, true, {0x83}, 6, dst);
    byte(static_cast<uint8_t>(imm));
}

void Assembler::cqo() {
    byte(0x48);
    byte(0x99);
}

void Assembler::idiv(Reg r) { emitRR(0, true, {0xF7}, 7, r); }
void Assembler::div(Reg r) { emitRR(0, true, {0xF7}, 6, r); }
void Assembler::neg(Reg r) { emitRR(0, true, {0xF7}, 3, r); }
void Assembler::not_(Reg r) { emitRR(0, true, {0xF7}, 2, r); }
void Assembler::shlCl(Reg r) { emitRR(0, true, {0xD3}, 4, r); }
void Assembler::shrCl(Reg r) { emitRR(0, true, {0xD3}, 5, r); }
void Assembler::sarCl(Reg r) { emitRR(0, true, {0xD3}, 7, r); }
void Assembler::shlImm(Reg r, uint8_t n) { emitRR(0, true, {0xC1}, 4, r); byte(n); }
void Assembler::shrImm(Reg r, uint8_t n) { emitRR(0, true, {0xC1}, 5, r); byte(n); }
void Assembler::sarImm(Reg r, uint8_t n) { emitRR(0, true, {0xC1}, 7, r); byte(n); }

void Assembler::setcc(Cond c, Reg r) { emitRR(0, false, {0x0F, static_cast<uint8_t>(0x90 | c)}, 0, r, true); }

void Assembler::testMem32(const Mem& m) { emitRM(0, false, {0x85}, RAX, m); }

// ───────────────────────────── Control flow ─────────────────────────────

void Assembler::callMem(const Mem& m) { emitRM(0, false, {0xFF}, 2, m); }
void Assembler::callR(Reg r) { emitRR(0, false, {0xFF}, 2, r); }

void Assembler::push(Reg r) {
    rex(false, 0, r, false);
    byte(static_cast<uint8_t>(0x50 + (r & 7)));
}

void Assembler::pop(Reg r) {
    rex(false, 0, r, false);
    byte(static_cast<uint8_t>(0x58 + (r & 7)));
}

void Assembler::ret() { byte(0xC3); }

void Assembler::nop(unsigned n) {
    for (unsigned i = 0; i < n; ++i) byte(0x90);
}

void Assembler::align(unsigned boundary) {
    // Recommended multi-byte NOP encodings (Intel SDM, "NOP" instruction).
    static const uint8_t nops[9][9] = {
        {0x90},
        {0x66, 0x90},
        {0x0F, 0x1F, 0x00},
        {0x0F, 0x1F, 0x40, 0x00},
        {0x0F, 0x1F, 0x44, 0x00, 0x00},
        {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
        {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
        {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    };
    unsigned pad = (boundary - pos() % boundary) % boundary;
    while (pad > 0) {
        unsigned n = pad > 9 ? 9 : pad;
        code.insert(code.end(), nops[n - 1], nops[n - 1] + n);
        pad -= n;
    }
}

void Assembler::lockXadd(const Mem& m, Reg src, unsigned size) {
    byte(0xF0);   // lock
    emitRM(0, size == 8, {0x0F, 0xC1}, src, m);
}

void Assembler::lockCmpxchg(const Mem& m, Reg src, unsigned size) {
    byte(0xF0);
    emitRM(0, size == 8, {0x0F, 0xB1}, src, m);
}

void Assembler::repMovsb() {
    byte(0xF3);
    byte(0xA4);
}

void Assembler::repStosb() {
    byte(0xF3);
    byte(0xAA);
}

// ───────────────────────────── SSE ─────────────────────────────

void Assembler::movsLoad(Xmm dst, const Mem& m, bool f32) { emitRM(f32 ? 0xF3 : 0xF2, false, {0x0F, 0x10}, dst, m); }
void Assembler::movsStore(const Mem& m, Xmm src, bool f32) { emitRM(f32 ? 0xF3 : 0xF2, false, {0x0F, 0x11}, src, m); }
void Assembler::movaps(Xmm dst, Xmm src) {
    if (dst != src) emitRR(0, false, {0x0F, 0x28}, dst, src);
}
void Assembler::movupsLoad(Xmm dst, const Mem& m) { emitRM(0, false, {0x0F, 0x10}, dst, m); }
void Assembler::movupsStore(const Mem& m, Xmm src) { emitRM(0, false, {0x0F, 0x11}, src, m); }
void Assembler::xorps(Xmm dst, Xmm src) { emitRR(0, false, {0x0F, 0x57}, dst, src); }
void Assembler::addS(Xmm dst, Xmm src, bool f32) { emitRR(f32 ? 0xF3 : 0xF2, false, {0x0F, 0x58}, dst, src); }
void Assembler::mulS(Xmm dst, Xmm src, bool f32) { emitRR(f32 ? 0xF3 : 0xF2, false, {0x0F, 0x59}, dst, src); }
void Assembler::subS(Xmm dst, Xmm src, bool f32) { emitRR(f32 ? 0xF3 : 0xF2, false, {0x0F, 0x5C}, dst, src); }
void Assembler::divS(Xmm dst, Xmm src, bool f32) { emitRR(f32 ? 0xF3 : 0xF2, false, {0x0F, 0x5E}, dst, src); }
void Assembler::sqrtS(Xmm dst, Xmm src, bool f32) { emitRR(f32 ? 0xF3 : 0xF2, false, {0x0F, 0x51}, dst, src); }

void Assembler::ucomis(Xmm a, Xmm b, bool f32) {
    if (f32) emitRR(0, false, {0x0F, 0x2E}, a, b);
    else emitRR(0x66, false, {0x0F, 0x2E}, a, b);
}

void Assembler::cvtsi2s(Xmm dst, Reg src, bool f32) { emitRR(f32 ? 0xF3 : 0xF2, true, {0x0F, 0x2A}, dst, src); }
void Assembler::cvtts2si(Reg dst, Xmm src, bool f32) { emitRR(f32 ? 0xF3 : 0xF2, true, {0x0F, 0x2C}, dst, src); }
void Assembler::cvtss2sd(Xmm dst, Xmm src) { emitRR(0xF3, false, {0x0F, 0x5A}, dst, src); }
void Assembler::cvtsd2ss(Xmm dst, Xmm src) { emitRR(0xF2, false, {0x0F, 0x5A}, dst, src); }
void Assembler::movqXR(Xmm dst, Reg src) { emitRR(0x66, true, {0x0F, 0x6E}, dst, src); }
void Assembler::movqRX(Reg dst, Xmm src) { emitRR(0x66, true, {0x0F, 0x7E}, src, dst); }

} // namespace ron::x64
