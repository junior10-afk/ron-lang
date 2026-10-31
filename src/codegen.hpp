#pragma once

#include "ast.hpp"
#include "sema.hpp"
#include "x64.hpp"

#include <map>
#include <string>
#include <vector>

namespace ron {

// Everything the PE writer needs to produce an executable.
struct Image {
    std::vector<uint8_t> text;
    std::vector<uint8_t> rdata;
    std::vector<uint8_t> data;
    std::vector<x64::Fixup> fixups;
    // Imports grouped by DLL; Section::Import fixup targets index `importSlots`.
    struct Import {
        std::string dll;
        std::string name;
    };
    std::vector<Import> importSlots;
    uint32_t entryOffset = 0;   // in .text
    // 64-bit absolute addresses stored in data (e.g. pointers inside type info tables).
    // The image has a fixed base, so the PE writer resolves them to final addresses.
    struct AbsFixup {
        x64::Section where;     // Rdata or Data
        uint32_t at;            // offset in that section
        x64::Section target;
        uint32_t offset;        // offset in the target section
    };
    std::vector<AbsFixup> absFixups;
};

// Single-pass code generator from the typed AST to x86-64 machine code.
//
// Conventions:
//  * Expression results: integers/bools/pointers in RAX, floats in XMM0,
//    structs and arrays as an address in RAX.
//  * Temporaries are frame slots (no push/pop), so RSP stays 16-byte aligned.
//  * Win64 calling convention for every call; parameter k lives in its home
//    slot at [rbp + 16 + 8k].
class CodeGen {
public:
    CodeGen(Sema& sema, TypeContext& types);
    Image generate();

private:
    using Reg = x64::Reg;
    using Xmm = x64::Xmm;
    using Mem = x64::Mem;

    // Data
    uint32_t addString(const std::string& s);
    uint32_t addF32(float v);
    uint32_t addF64(double v);
    uint32_t importSlot(const std::string& dll, const std::string& name);
    void layoutGlobals();

    // Functions
    void genFunction(FuncDecl* f);
    void genEntryStub();

    // Frame slots
    int32_t allocSlot(uint32_t size, uint32_t align);
    int32_t allocTemp8() { return allocSlot(8, 8); }

    // Statements
    void genStmt(Stmt* s);
    // Jumps to `target` when `cond` evaluates to `jumpWhen`; avoids materializing bools.
    void genBranch(Expr* cond, x64::Label target, bool jumpWhen);

    // Expressions
    void genExpr(Expr* e);
    void genAddr(Expr* e);
    void genBinary(BinaryExpr* e);
    void genUnary(UnaryExpr* e);
    void genAssign(AssignExpr* e);
    void genCallTo(FuncDecl* f, const std::vector<Expr*>& args);
    // Shared by direct and indirect calls (exactly one of `direct` / `calleeValue` is set).
    void genCall(const std::vector<Type*>& paramTypes, Type* rt, const std::vector<Expr*>& args, FuncDecl* direct,
                 Expr* calleeValue);
    void genSlice(SliceExpr* e);
    void genVariant(VariantExpr* v);
    void genTry(TryExpr* e);
    void genMatch(MatchStmt* m);
    void genPack(PackExpr* p);
    uint32_t typeInfo(Type* t);
    uint32_t rdataAlloc(uint32_t size, uint32_t align);
    void rdataPointer(uint32_t at, uint32_t targetOffset);
    std::map<Type*, uint32_t> typeInfos_;
    void finishReturn(bool hasValue);
    void genFunctionAddress(FuncDecl* f);
    std::vector<std::pair<size_t, FuncDecl*>> funcRefs_;   // fixup index -> function whose address is taken
    void genCast(CastExpr* e);
    void genStructLit(StructLitExpr* e);
    void genIncDec(UnaryExpr* e);
    Mem genMem(Expr* e);                              // lvalue / aggregate → memory operand
    Mem genIndexMem(IndexExpr* e);
    bool stableMem(Expr* e, Mem& out);                // address without emitting code, if possible
    x64::Cond genCompareFlags(BinaryExpr* e);
    // Evaluate both operands; returns the registers holding them. With lhsInAcc the
    // left operand is in RAX / XMM0 (ready to be overwritten by the result).
    std::pair<Reg, Reg> genOperandsInt(Expr* lhs, Expr* rhs, bool lhsInAcc = true);
    std::pair<Xmm, Xmm> genOperandsFloat(Expr* lhs, Expr* rhs, bool lhsInAcc = true);
    void emitIntOp(BinaryOp op, Type* t, Reg rhs);   // RAX = RAX op rhs
    void emitFloatOp(BinaryOp op, bool f32, Xmm dst, Xmm src);
    bool emitIntOpImm(BinaryOp op, Type* t, Reg dst, Expr* rhs);   // dst op= literal, if encodable

    // Register allocation
    void allocateRegisters(FuncDecl* f);
    int regOf(Expr* e) const;      // GPR holding this integer/pointer variable, or -1
    int xmmOf(Expr* e) const;      // XMM register holding this float variable, or -1
    bool isSimple(Expr* e) const;
    bool isSimpleFloat(Expr* e) const;

    // Helpers
    bool simpleInto(Expr* e, Reg r);    // load trivially computable operands without temporaries
    bool simpleFloatInto(Expr* e, Xmm x);
    void loadValue(Type* t, const Mem& m);           // into RAX / XMM0 / address in RAX
    void storeValue(Type* t, const Mem& m);          // from RAX / XMM0 / address in RAX
    void normalize(Type* t, Reg r = x64::RAX);       // re-extend a narrow integer after arithmetic
    void copyMem(const Mem& dst, const Mem& src, uint32_t size);
    void zeroMem(const Mem& dst, uint32_t size);
    Mem localMem(LocalVar* v) const { return Mem::at(x64::RBP, v->frameOffset); }
    static void addDisp(Mem& m, int32_t d) {
        if (m.rip) m.target += static_cast<uint32_t>(d);
        else m.disp += d;
    }
    static bool isRegSize(uint32_t size) { return size == 1 || size == 2 || size == 4 || size == 8; }
    static bool passedByRef(Type* t) { return t->isAggregate() && !isRegSize(t->size); }
    static x64::Cond condFor(BinaryOp op, bool isSigned, bool isFloat);

    Sema& sema_;
    TypeContext& types_;
    x64::Assembler as_;
    Image image_;

    std::map<FuncDecl*, x64::Label> funcLabels_;
    std::map<std::string, uint32_t> strings_;
    std::map<uint32_t, uint32_t> f32s_;
    std::map<uint64_t, uint32_t> f64s_;
    std::map<std::pair<std::string, std::string>, uint32_t> imports_;

    // Per-function state
    FuncDecl* func_ = nullptr;
    int32_t frameSize_ = 0;
    int32_t maxFrame_ = 0;
    int32_t maxOutgoing_ = 32;
    x64::Label epilogue_ = 0;
    bool hiddenReturn_ = false;
    std::vector<std::pair<Reg, int32_t>> savedGprs_;   // callee-saved registers and their save slots
    std::vector<std::pair<Xmm, int32_t>> savedXmms_;
    struct Loop {
        x64::Label breakLabel;
        x64::Label continueLabel;
        std::string label;
        size_t deferDepth;   // defer frames that were active when the loop started
        bool isSwitch;       // switches accept `break` but not `continue`
    };
    std::vector<Loop> loops_;
    // One frame per block being generated; each holds its `defer` statements in order.
    std::vector<std::vector<Stmt*>> deferFrames_;
    void emitDefers(size_t downTo);                 // runs pending defers of frames above `downTo`
    void genScope(const std::vector<Stmt*>& stmts);  // a block: own defer frame
    int findLoop(const std::string& label, bool isContinue) const;
    void genArrayLit(ArrayLitExpr* e);
};

} // namespace ron
