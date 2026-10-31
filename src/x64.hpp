#pragma once

// Minimal x86-64 machine code encoder: just the instructions Ron's code
// generator needs, written byte by byte from the Intel manual encodings.

#include <cstdint>
#include <vector>

namespace ron::x64 {

enum Reg : uint8_t {
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15,
};

// XMM registers share the same 0..15 numbering.
enum Xmm : uint8_t { XMM0 = 0, XMM1, XMM2, XMM3, XMM4, XMM5 };

// Condition codes (low nibble of Jcc / SETcc opcodes).
enum Cond : uint8_t {
    CC_O = 0x0, CC_NO = 0x1, CC_B = 0x2, CC_AE = 0x3, CC_E = 0x4, CC_NE = 0x5, CC_BE = 0x6, CC_A = 0x7,
    CC_S = 0x8, CC_NS = 0x9, CC_P = 0xA, CC_NP = 0xB, CC_L = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF,
};

inline Cond invert(Cond c) { return static_cast<Cond>(c ^ 1); }

// Sections a RIP-relative operand can point into.
enum class Section : uint8_t { Text, Rdata, Data, Import };

constexpr uint8_t NO_INDEX = 0xFF;

// Memory operand: [base + index*scale + disp] or [rip + target-in-section].
struct Mem {
    bool rip = false;
    Reg base = RAX;
    int32_t disp = 0;
    Section section = Section::Rdata;
    uint32_t target = 0;   // offset in section (or import slot index)
    uint8_t index = NO_INDEX;
    uint8_t scale = 1;     // 1, 2, 4 or 8

    static Mem at(Reg base, int32_t disp = 0) { return Mem{false, base, disp, Section::Rdata, 0}; }
    static Mem indexed(Reg base, Reg index, uint8_t scale, int32_t disp = 0) {
        return Mem{false, base, disp, Section::Rdata, 0, static_cast<uint8_t>(index), scale};
    }
    static Mem ripRel(Section s, uint32_t target) { return Mem{true, RAX, 0, s, target}; }
    bool hasIndex() const { return index != NO_INDEX; }
};

// A RIP-relative disp32 to patch once section addresses are known.
struct Fixup {
    uint32_t at;        // offset of the disp32 in .text
    Section section;
    uint32_t target;
};

using Label = uint32_t;

class Assembler {
public:
    std::vector<uint8_t> code;
    std::vector<Fixup> fixups;

    size_t pos() const { return code.size(); }

    // Labels
    Label newLabel();
    void bind(Label l);
    void jmp(Label l);
    void jcc(Cond c, Label l);
    void call(Label l);
    uint32_t labelPos(Label l) const { return static_cast<uint32_t>(labels_[l].pos); }

    // Integer moves
    void movRR(Reg dst, Reg src);                 // mov dst, src (64-bit)
    void movRR32(Reg dst, Reg src);               // mov dst32, src32 (zero-extends)
    void movRI(Reg dst, uint64_t imm);
    void load(Reg dst, const Mem& m, unsigned size, bool signExtend);
    void store(const Mem& m, Reg src, unsigned size);
    void storeImm32(const Mem& m, int32_t imm, unsigned size);   // size 4 or 8
    void lea(Reg dst, const Mem& m);
    void movsx(Reg dst, Reg src, unsigned fromSize);   // sign-extend low bytes to 64-bit
    void movzx(Reg dst, Reg src, unsigned fromSize);   // zero-extend low bytes to 64-bit

    // Integer arithmetic (64-bit)
    void add(Reg dst, Reg src);
    void sub(Reg dst, Reg src);
    void and_(Reg dst, Reg src);
    void or_(Reg dst, Reg src);
    void xor_(Reg dst, Reg src);
    void cmp(Reg a, Reg b);
    void test(Reg a, Reg b);
    void imul(Reg dst, Reg src);
    void imulImm(Reg dst, Reg src, int32_t imm);
    void addImm(Reg dst, int32_t imm);
    void subImm(Reg dst, int32_t imm);
    void subImm32Patchable(Reg dst, uint32_t& immOffset);   // always uses a 32-bit immediate
    void cmpImm(Reg a, int32_t imm);
    void andImm(Reg dst, int32_t imm);
    void orImm(Reg dst, int32_t imm);
    void xorImm(Reg dst, int32_t imm);
    void xorImm8(Reg dst, int8_t imm);
    void cqo();
    void idiv(Reg r);
    void div(Reg r);
    void neg(Reg r);
    void not_(Reg r);
    void shlCl(Reg r);
    void shrCl(Reg r);
    void sarCl(Reg r);
    void shlImm(Reg r, uint8_t n);
    void shrImm(Reg r, uint8_t n);
    void sarImm(Reg r, uint8_t n);
    void setcc(Cond c, Reg r);   // r8 = cond ? 1 : 0
    void testMem32(const Mem& m);   // test [m], eax (used for stack probes)

    // Control flow
    void callMem(const Mem& m);
    void callR(Reg r);
    void push(Reg r);
    void pop(Reg r);
    void ret();
    void nop(unsigned n = 1);
    void align(unsigned boundary);   // multi-byte NOPs up to the next boundary
    void repMovsb();
    void repStosb();

    // SSE scalar floats (f32 = single precision when true)
    void movsLoad(Xmm dst, const Mem& m, bool f32);
    void movsStore(const Mem& m, Xmm src, bool f32);
    void movaps(Xmm dst, Xmm src);
    void movupsLoad(Xmm dst, const Mem& m);     // full 128-bit (callee-saved xmm6-15)
    void movupsStore(const Mem& m, Xmm src);
    void xorps(Xmm dst, Xmm src);
    void addS(Xmm dst, Xmm src, bool f32);
    void subS(Xmm dst, Xmm src, bool f32);
    void mulS(Xmm dst, Xmm src, bool f32);
    void divS(Xmm dst, Xmm src, bool f32);
    void sqrtS(Xmm dst, Xmm src, bool f32);
    void ucomis(Xmm a, Xmm b, bool f32);
    void cvtsi2s(Xmm dst, Reg src, bool f32);     // from 64-bit integer
    void cvtts2si(Reg dst, Xmm src, bool f32);    // to 64-bit integer, truncating
    void cvtss2sd(Xmm dst, Xmm src);
    void cvtsd2ss(Xmm dst, Xmm src);
    void movqXR(Xmm dst, Reg src);
    void movqRX(Reg dst, Xmm src);

    // Raw bytes
    void byte(uint8_t b) { code.push_back(b); }
    void dword(uint32_t v);
    void qword(uint64_t v);
    void patch32(uint32_t at, uint32_t v);

private:
    struct LabelInfo {
        int64_t pos = -1;
        std::vector<uint32_t> refs;
    };

    void rex(bool w, unsigned reg, unsigned base, bool forceForByteRegs, unsigned index = 0);
    void aluImm(unsigned ext, Reg dst, int32_t imm);
    void modrmMem(unsigned reg, const Mem& m);
    void emitRM(uint8_t legacy, bool w, std::initializer_list<uint8_t> opcode, unsigned reg, const Mem& m,
                bool byteRegs = false);
    void emitRR(uint8_t legacy, bool w, std::initializer_list<uint8_t> opcode, unsigned reg, unsigned rm,
                bool byteRegs = false);
    void rel32To(Label l);

    std::vector<LabelInfo> labels_;
};

} // namespace ron::x64
