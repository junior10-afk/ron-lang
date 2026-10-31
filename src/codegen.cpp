#include "codegen.hpp"

#include <algorithm>
#include <cstring>

namespace ron {

using namespace x64;

namespace {

constexpr Reg kArgRegs[4] = {RCX, RDX, R8, R9};

int32_t alignUp(int32_t v, int32_t a) { return (v + a - 1) / a * a; }

bool isSignedInt(const Type* t) { return t->isInt() && t->isSigned; }

} // namespace

CodeGen::CodeGen(Sema& sema, TypeContext& types) : sema_(sema), types_(types) {}

// ───────────────────────────── Data ─────────────────────────────

uint32_t CodeGen::addString(const std::string& s) {
    auto it = strings_.find(s);
    if (it != strings_.end()) return it->second;
    auto off = static_cast<uint32_t>(image_.rdata.size());
    image_.rdata.insert(image_.rdata.end(), s.begin(), s.end());
    image_.rdata.push_back(0);
    strings_[s] = off;
    return off;
}

uint32_t CodeGen::addF32(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    auto it = f32s_.find(bits);
    if (it != f32s_.end()) return it->second;
    while (image_.rdata.size() % 4) image_.rdata.push_back(0);
    auto off = static_cast<uint32_t>(image_.rdata.size());
    for (int i = 0; i < 4; ++i) image_.rdata.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    f32s_[bits] = off;
    return off;
}

uint32_t CodeGen::addF64(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    auto it = f64s_.find(bits);
    if (it != f64s_.end()) return it->second;
    while (image_.rdata.size() % 8) image_.rdata.push_back(0);
    auto off = static_cast<uint32_t>(image_.rdata.size());
    for (int i = 0; i < 8; ++i) image_.rdata.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    f64s_[bits] = off;
    return off;
}

uint32_t CodeGen::importSlot(const std::string& dll, const std::string& name) {
    auto key = std::make_pair(dll, name);
    auto it = imports_.find(key);
    if (it != imports_.end()) return it->second;
    auto idx = static_cast<uint32_t>(image_.importSlots.size());
    image_.importSlots.push_back({dll, name});
    imports_[key] = idx;
    return idx;
}

void CodeGen::layoutGlobals() {
    auto& data = image_.data;
    for (GlobalVar* g : sema_.globals) {
        Type* t = g->type;
        while (data.size() % t->align) data.push_back(0);
        g->dataOffset = static_cast<uint32_t>(data.size());
        std::vector<uint8_t> bytes(t->size, 0);
        if (g->hasValue) {
            uint64_t raw = g->intValue;
            if (g->isFloatValue) {
                if (t->size == 4) {
                    float f = static_cast<float>(g->floatValue);
                    uint32_t b;
                    std::memcpy(&b, &f, 4);
                    raw = b;
                } else {
                    std::memcpy(&raw, &g->floatValue, 8);
                }
            }
            for (uint32_t i = 0; i < t->size && i < 8; ++i) bytes[i] = static_cast<uint8_t>(raw >> (8 * i));
        }
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
}

// ───────────────────────────── Driver ─────────────────────────────

Image CodeGen::generate() {
    for (FuncDecl* f : sema_.functions) funcLabels_[f] = as_.newLabel();
    layoutGlobals();
    genEntryStub();
    for (FuncDecl* f : sema_.functions) genFunction(f);
    image_.text = std::move(as_.code);
    image_.fixups = std::move(as_.fixups);
    return std::move(image_);
}

void CodeGen::genEntryStub() {
    image_.entryOffset = static_cast<uint32_t>(as_.pos());
    as_.subImm(RSP, 40);   // shadow space + realign (entry rsp is 8 mod 16)
    as_.call(funcLabels_[sema_.mainFunc]);
    if (sema_.mainFunc->returnType->isVoid()) as_.movRI(RCX, 0);
    else as_.movRR32(RCX, RAX);
    as_.callMem(Mem::ripRel(Section::Import, importSlot("kernel32.dll", "ExitProcess")));
    as_.byte(0xCC);   // int3: never reached
}

int32_t CodeGen::allocSlot(uint32_t size, uint32_t align) {
    int32_t a = static_cast<int32_t>(std::max<uint32_t>(align, 1));
    frameSize_ = alignUp(frameSize_ + static_cast<int32_t>(size), a);
    maxFrame_ = std::max(maxFrame_, frameSize_);
    return -frameSize_;
}

void CodeGen::genFunction(FuncDecl* f) {
    func_ = f;
    frameSize_ = 0;
    maxFrame_ = 0;
    maxOutgoing_ = 32;
    loops_.clear();
    hiddenReturn_ = passedByRef(f->returnType);

    as_.bind(funcLabels_[f]);
    as_.push(RBP);
    as_.movRR(RBP, RSP);
    uint32_t subAt = static_cast<uint32_t>(as_.pos());
    uint32_t frameImm = 0;
    as_.subImm32Patchable(RSP, frameImm);   // 7 bytes, patched once the frame size is known

    // Home register parameters into their shadow-space slots.
    int k0 = hiddenReturn_ ? 1 : 0;
    if (hiddenReturn_) as_.store(Mem::at(RBP, 16), RCX, 8);
    for (size_t i = 0; i < f->paramVars.size(); ++i) {
        int k = static_cast<int>(i) + k0;
        LocalVar* v = f->paramVars[i];
        v->frameOffset = 16 + 8 * k;
        v->indirect = passedByRef(v->type);
        if (k < 4) {
            if (v->type->isFloat()) as_.movsStore(localMem(v), static_cast<Xmm>(k), v->type->size == 4);
            else as_.store(localMem(v), kArgRegs[k], 8);
        }
    }

    epilogue_ = as_.newLabel();
    genStmt(f->body);
    as_.movRI(RAX, 0);   // falling off the end returns 0 (main's implicit exit code)
    as_.bind(epilogue_);
    as_.movRR(RSP, RBP);
    as_.pop(RBP);
    as_.ret();

    int32_t frame = alignUp(maxFrame_, 16) + alignUp(maxOutgoing_, 16);
    if (frame <= 4096) {
        as_.patch32(frameImm, static_cast<uint32_t>(frame));
        return;
    }
    // Large frame: touch each stack page in order so the guard page grows the stack.
    // The 7-byte `sub rsp, imm32` becomes `jmp probe; nop; nop`.
    uint32_t probe = static_cast<uint32_t>(as_.pos());
    for (int32_t off = 4096; off < frame; off += 4096) as_.testMem32(Mem::at(RSP, -off));
    as_.subImm(RSP, frame);
    as_.byte(0xE9);
    as_.dword(static_cast<uint32_t>(static_cast<int32_t>(subAt + 7) - static_cast<int32_t>(as_.pos() + 4)));
    as_.code[subAt] = 0xE9;
    as_.patch32(subAt + 1, probe - (subAt + 5));
    as_.code[subAt + 5] = 0x90;
    as_.code[subAt + 6] = 0x90;
}

// ───────────────────────────── Statements ─────────────────────────────

void CodeGen::genStmt(Stmt* s) {
    switch (s->kind) {
    case StmtKind::Block:
        for (Stmt* c : s->as<BlockStmt>()->stmts) genStmt(c);
        break;
    case StmtKind::VarDecl: {
        auto* v = s->as<VarDeclStmt>();
        LocalVar* var = v->var;
        var->frameOffset = allocSlot(var->type->size, var->type->align);
        int32_t mark = frameSize_;
        if (v->init) {
            genExpr(v->init);
            storeValue(var->type, localMem(var));
        } else {
            zeroMem(RBP, var->frameOffset, var->type->size);
        }
        frameSize_ = mark;
        break;
    }
    case StmtKind::Expr: {
        int32_t mark = frameSize_;
        genExpr(s->as<ExprStmt>()->expr);
        frameSize_ = mark;
        break;
    }
    case StmtKind::If: {
        auto* i = s->as<IfStmt>();
        Label elseL = as_.newLabel();
        Label endL = as_.newLabel();
        int32_t mark = frameSize_;
        genBranch(i->cond, elseL, false);
        frameSize_ = mark;
        genStmt(i->thenStmt);
        if (i->elseStmt) as_.jmp(endL);
        as_.bind(elseL);
        if (i->elseStmt) genStmt(i->elseStmt);
        as_.bind(endL);
        break;
    }
    case StmtKind::While: {
        auto* w = s->as<WhileStmt>();
        Label top = as_.newLabel();
        Label end = as_.newLabel();
        as_.bind(top);
        int32_t mark = frameSize_;
        genBranch(w->cond, end, false);
        frameSize_ = mark;
        loops_.push_back({end, top});
        genStmt(w->body);
        loops_.pop_back();
        as_.jmp(top);
        as_.bind(end);
        break;
    }
    case StmtKind::For: {
        auto* f = s->as<ForStmt>();
        if (f->init) genStmt(f->init);
        Label top = as_.newLabel();
        Label cont = as_.newLabel();
        Label end = as_.newLabel();
        as_.bind(top);
        int32_t mark = frameSize_;
        if (f->cond) genBranch(f->cond, end, false);
        frameSize_ = mark;
        loops_.push_back({end, cont});
        genStmt(f->body);
        loops_.pop_back();
        as_.bind(cont);
        if (f->step) {
            mark = frameSize_;
            genExpr(f->step);
            frameSize_ = mark;
        }
        as_.jmp(top);
        as_.bind(end);
        break;
    }
    case StmtKind::Return: {
        auto* r = s->as<ReturnStmt>();
        if (r->value) {
            int32_t mark = frameSize_;
            genExpr(r->value);
            Type* rt = func_->returnType;
            if (rt->isAggregate()) {
                if (hiddenReturn_) {
                    as_.load(R10, Mem::at(RBP, 16), 8, false);
                    copyMem(R10, 0, RAX, 0, rt->size);
                    as_.movRR(RAX, R10);
                } else {
                    as_.load(RAX, Mem::at(RAX, 0), rt->size, false);
                }
            }
            frameSize_ = mark;
        } else if (func_->returnType->isInt()) {
            as_.movRI(RAX, 0);
        }
        as_.jmp(epilogue_);
        break;
    }
    case StmtKind::Break:
        as_.jmp(loops_.back().breakLabel);
        break;
    case StmtKind::Continue:
        as_.jmp(loops_.back().continueLabel);
        break;
    }
}

void CodeGen::genBranch(Expr* cond, Label target, bool jumpWhen) {
    if (cond->kind == ExprKind::BoolLit) {
        if (cond->as<BoolLitExpr>()->value == jumpWhen) as_.jmp(target);
        return;
    }
    if (cond->kind == ExprKind::Unary && cond->as<UnaryExpr>()->op == UnaryOp::Not) {
        genBranch(cond->as<UnaryExpr>()->operand, target, !jumpWhen);
        return;
    }
    if (cond->kind == ExprKind::Binary) {
        auto* b = cond->as<BinaryExpr>();
        if (b->op == BinaryOp::LogAnd || b->op == BinaryOp::LogOr) {
            bool isAnd = b->op == BinaryOp::LogAnd;
            if (isAnd != jumpWhen) {
                // and/false: jump if either is false.  or/true: jump if either is true.
                genBranch(b->lhs, target, jumpWhen);
                genBranch(b->rhs, target, jumpWhen);
            } else {
                Label skip = as_.newLabel();
                genBranch(b->lhs, skip, !jumpWhen);
                genBranch(b->rhs, target, jumpWhen);
                as_.bind(skip);
            }
            return;
        }
        if (!b->overload && (b->op == BinaryOp::Eq || b->op == BinaryOp::Ne || b->op == BinaryOp::Lt ||
                             b->op == BinaryOp::Le || b->op == BinaryOp::Gt || b->op == BinaryOp::Ge)) {
            Cond c = genCompareFlags(b);
            as_.jcc(jumpWhen ? c : invert(c), target);
            return;
        }
    }
    genExpr(cond);
    as_.test(RAX, RAX);
    as_.jcc(jumpWhen ? CC_NE : CC_E, target);
}

// ───────────────────────────── Helpers ─────────────────────────────

Cond CodeGen::condFor(BinaryOp op, bool isSigned, bool isFloat) {
    bool s = isSigned && !isFloat;
    switch (op) {
    case BinaryOp::Eq: return CC_E;
    case BinaryOp::Ne: return CC_NE;
    case BinaryOp::Lt: return s ? CC_L : CC_B;
    case BinaryOp::Le: return s ? CC_LE : CC_BE;
    case BinaryOp::Gt: return s ? CC_G : CC_A;
    case BinaryOp::Ge: return s ? CC_GE : CC_AE;
    default: return CC_E;
    }
}

void CodeGen::normalize(Type* t, Reg r) {
    if (t->isBool()) {
        as_.movzx(r, r, 1);
        return;
    }
    if (!t->isInt() || t->size == 8) return;
    if (t->isSigned) as_.movsx(r, r, t->size);
    else as_.movzx(r, r, t->size);
}

void CodeGen::loadValue(Type* t, const Mem& m) {
    if (t->isFloat()) as_.movsLoad(XMM0, m, t->size == 4);
    else if (t->isAggregate()) as_.lea(RAX, m);
    else as_.load(RAX, m, t->size, isSignedInt(t));
}

void CodeGen::storeValue(Type* t, const Mem& m) {
    if (t->isFloat()) {
        as_.movsStore(m, XMM0, t->size == 4);
    } else if (t->isAggregate()) {
        if (m.rip) {
            as_.lea(R10, m);
            copyMem(R10, 0, RAX, 0, t->size);
        } else {
            copyMem(m.base, m.disp, RAX, 0, t->size);
        }
    } else {
        as_.store(m, RAX, t->size);
    }
}

void CodeGen::copyMem(Reg dst, int32_t dstDisp, Reg src, int32_t srcDisp, uint32_t size) {
    if (size <= 128) {
        uint32_t i = 0;
        for (unsigned chunk : {8u, 4u, 2u, 1u}) {
            for (; i + chunk <= size; i += chunk) {
                as_.load(R11, Mem::at(src, srcDisp + static_cast<int32_t>(i)), chunk, false);
                as_.store(Mem::at(dst, dstDisp + static_cast<int32_t>(i)), R11, chunk);
            }
        }
        return;
    }
    as_.push(RSI);
    as_.push(RDI);
    as_.lea(RSI, Mem::at(src, srcDisp));
    as_.lea(RDI, Mem::at(dst, dstDisp));
    as_.movRI(RCX, size);
    as_.repMovsb();
    as_.pop(RDI);
    as_.pop(RSI);
}

void CodeGen::zeroMem(Reg base, int32_t disp, uint32_t size) {
    if (size <= 128) {
        as_.movRI(R11, 0);
        uint32_t i = 0;
        for (unsigned chunk : {8u, 4u, 2u, 1u})
            for (; i + chunk <= size; i += chunk) as_.store(Mem::at(base, disp + static_cast<int32_t>(i)), R11, chunk);
        return;
    }
    as_.push(RDI);
    as_.lea(RDI, Mem::at(base, disp));
    as_.movRI(RCX, size);
    as_.movRI(RAX, 0);
    as_.repStosb();
    as_.pop(RDI);
}

bool CodeGen::simpleInto(Expr* e, Reg r) {
    if (e->kind == ExprKind::IntLit) {
        as_.movRI(r, e->as<IntLitExpr>()->value);
        return true;
    }
    if (e->kind == ExprKind::NullLit) {
        as_.movRI(r, 0);
        return true;
    }
    if (e->kind == ExprKind::BoolLit) {
        as_.movRI(r, e->as<BoolLitExpr>()->value ? 1 : 0);
        return true;
    }
    Type* loadType = e->type;
    Expr* x = e;
    // An implicit int widening of a variable is just a load with the source's extension.
    if (e->kind == ExprKind::Cast && !e->as<CastExpr>()->target) {
        Expr* op = e->as<CastExpr>()->operand;
        if (op->type->isInt() && e->type->isInt() && op->type->size <= e->type->size &&
            !(op->type->isSigned && !e->type->isSigned)) {
            x = op;
            loadType = op->type;
        } else {
            return false;
        }
    }
    if (x->kind != ExprKind::Ident || !loadType->isScalar()) return false;
    auto* id = x->as<IdentExpr>();
    if (id->local && !id->local->indirect) {
        as_.load(r, localMem(id->local), loadType->size, isSignedInt(loadType));
        return true;
    }
    if (id->global) {
        as_.load(r, Mem::ripRel(Section::Data, id->global->dataOffset), loadType->size, isSignedInt(loadType));
        return true;
    }
    return false;
}

bool CodeGen::simpleFloatInto(Expr* e, Xmm x) {
    bool f32 = e->type->size == 4;
    if (e->kind == ExprKind::FloatLit) {
        double v = e->as<FloatLitExpr>()->value;
        as_.movsLoad(x, Mem::ripRel(Section::Rdata, f32 ? addF32(static_cast<float>(v)) : addF64(v)), f32);
        return true;
    }
    if (e->kind == ExprKind::Ident) {
        auto* id = e->as<IdentExpr>();
        if (id->local && !id->local->indirect) {
            as_.movsLoad(x, localMem(id->local), f32);
            return true;
        }
        if (id->global) {
            as_.movsLoad(x, Mem::ripRel(Section::Data, id->global->dataOffset), f32);
            return true;
        }
    }
    return false;
}

void CodeGen::genOperandsInt(Expr* lhs, Expr* rhs) {
    genExpr(lhs);
    if (simpleInto(rhs, RCX)) return;
    int32_t t = allocTemp8();
    as_.store(Mem::at(RBP, t), RAX, 8);
    genExpr(rhs);
    as_.movRR(RCX, RAX);
    as_.load(RAX, Mem::at(RBP, t), 8, false);
}

void CodeGen::genOperandsFloat(Expr* lhs, Expr* rhs) {
    genExpr(lhs);
    if (simpleFloatInto(rhs, XMM1)) return;
    bool f32 = lhs->type->size == 4;
    int32_t t = allocTemp8();
    as_.movsStore(Mem::at(RBP, t), XMM0, f32);
    genExpr(rhs);
    as_.movaps(XMM1, XMM0);
    as_.movsLoad(XMM0, Mem::at(RBP, t), f32);
}

void CodeGen::emitIntOp(BinaryOp op, Type* t) {
    bool s = isSignedInt(t);
    switch (op) {
    case BinaryOp::Add: as_.add(RAX, RCX); break;
    case BinaryOp::Sub: as_.sub(RAX, RCX); break;
    case BinaryOp::Mul: as_.imul(RAX, RCX); break;
    case BinaryOp::Div:
    case BinaryOp::Mod:
        if (s) {
            as_.cqo();
            as_.idiv(RCX);
        } else {
            as_.movRI(RDX, 0);
            as_.div(RCX);
        }
        if (op == BinaryOp::Mod) as_.movRR(RAX, RDX);
        break;
    case BinaryOp::BitAnd: as_.and_(RAX, RCX); break;
    case BinaryOp::BitOr: as_.or_(RAX, RCX); break;
    case BinaryOp::BitXor: as_.xor_(RAX, RCX); break;
    case BinaryOp::Shl: as_.shlCl(RAX); break;
    case BinaryOp::Shr:
        if (s) as_.sarCl(RAX);
        else as_.shrCl(RAX);
        break;
    default: break;
    }
    normalize(t);
}

// ───────────────────────────── Expressions ─────────────────────────────

void CodeGen::genExpr(Expr* e) {
    switch (e->kind) {
    case ExprKind::IntLit:
        as_.movRI(RAX, e->as<IntLitExpr>()->value);
        break;
    case ExprKind::FloatLit: {
        double v = e->as<FloatLitExpr>()->value;
        bool f32 = e->type->size == 4;
        as_.movsLoad(XMM0, Mem::ripRel(Section::Rdata, f32 ? addF32(static_cast<float>(v)) : addF64(v)), f32);
        break;
    }
    case ExprKind::StringLit:
        as_.lea(RAX, Mem::ripRel(Section::Rdata, addString(e->as<StringLitExpr>()->value)));
        break;
    case ExprKind::BoolLit:
        as_.movRI(RAX, e->as<BoolLitExpr>()->value ? 1 : 0);
        break;
    case ExprKind::NullLit:
        as_.movRI(RAX, 0);
        break;
    case ExprKind::Ident: {
        auto* id = e->as<IdentExpr>();
        if (id->local) {
            if (id->local->indirect) as_.load(RAX, localMem(id->local), 8, false);
            else loadValue(e->type, localMem(id->local));
        } else {
            loadValue(e->type, Mem::ripRel(Section::Data, id->global->dataOffset));
        }
        break;
    }
    case ExprKind::Unary:
        genUnary(e->as<UnaryExpr>());
        break;
    case ExprKind::Binary:
        genBinary(e->as<BinaryExpr>());
        break;
    case ExprKind::Assign:
        genAssign(e->as<AssignExpr>());
        break;
    case ExprKind::Ternary: {
        auto* t = e->as<TernaryExpr>();
        Label elseL = as_.newLabel();
        Label endL = as_.newLabel();
        genBranch(t->cond, elseL, false);
        genExpr(t->thenExpr);
        as_.jmp(endL);
        as_.bind(elseL);
        genExpr(t->elseExpr);
        as_.bind(endL);
        break;
    }
    case ExprKind::Call: {
        auto* c = e->as<CallExpr>();
        if (c->intrinsic == Intrinsic::SqrtF32 || c->intrinsic == Intrinsic::SqrtF64) {
            genExpr(c->args[0]);
            as_.sqrtS(XMM0, XMM0, c->intrinsic == Intrinsic::SqrtF32);
        } else {
            genCallTo(c->func, c->args);
        }
        break;
    }
    case ExprKind::Index:
        genIndexAddr(e->as<IndexExpr>());
        loadValue(e->type, Mem::at(RAX, 0));
        break;
    case ExprKind::Member: {
        auto* m = e->as<MemberExpr>();
        genMemberBase(m);
        loadValue(e->type, Mem::at(RAX, static_cast<int32_t>(m->offset)));
        break;
    }
    case ExprKind::Cast:
        genCast(e->as<CastExpr>());
        break;
    case ExprKind::StructLit:
        genStructLit(e->as<StructLitExpr>());
        break;
    case ExprKind::Path:
    case ExprKind::Sizeof:
        break;   // folded away by sema
    }
}

void CodeGen::genAddr(Expr* e) {
    switch (e->kind) {
    case ExprKind::Ident: {
        auto* id = e->as<IdentExpr>();
        if (id->local) {
            if (id->local->indirect) as_.load(RAX, localMem(id->local), 8, false);
            else as_.lea(RAX, localMem(id->local));
        } else {
            as_.lea(RAX, Mem::ripRel(Section::Data, id->global->dataOffset));
        }
        break;
    }
    case ExprKind::Unary:
        if (e->as<UnaryExpr>()->op == UnaryOp::Deref) {
            genExpr(e->as<UnaryExpr>()->operand);
            break;
        }
        genExpr(e);
        break;
    case ExprKind::Index:
        genIndexAddr(e->as<IndexExpr>());
        break;
    case ExprKind::Member: {
        auto* m = e->as<MemberExpr>();
        genMemberBase(m);
        as_.addImm(RAX, static_cast<int32_t>(m->offset));
        break;
    }
    default:
        genExpr(e);   // aggregate rvalues evaluate to the address of a temporary
        break;
    }
}

void CodeGen::genMemberBase(MemberExpr* m) {
    if (m->throughPointer) genExpr(m->base);
    else genAddr(m->base);
}

void CodeGen::genIndexAddr(IndexExpr* e) {
    Type* bt = e->base->type;
    uint32_t elemSize = bt->elem->size;
    if (bt->isArray()) genAddr(e->base);
    else genExpr(e->base);

    if (e->index->kind == ExprKind::IntLit) {
        int64_t off = static_cast<int64_t>(e->index->as<IntLitExpr>()->value) * elemSize;
        if (off >= INT32_MIN && off <= INT32_MAX) {
            as_.addImm(RAX, static_cast<int32_t>(off));
            return;
        }
    }
    if (!simpleInto(e->index, RCX)) {
        int32_t t = allocTemp8();
        as_.store(Mem::at(RBP, t), RAX, 8);
        genExpr(e->index);
        as_.movRR(RCX, RAX);
        as_.load(RAX, Mem::at(RBP, t), 8, false);
    }
    if (elemSize != 1) as_.imulImm(RCX, RCX, static_cast<int32_t>(elemSize));
    as_.add(RAX, RCX);
}

Cond CodeGen::genCompareFlags(BinaryExpr* e) {
    Type* t = e->lhs->type;
    if (t->isFloat()) {
        genOperandsFloat(e->lhs, e->rhs);
        as_.ucomis(XMM0, XMM1, t->size == 4);
        return condFor(e->op, false, true);
    }
    genOperandsInt(e->lhs, e->rhs);
    as_.cmp(RAX, RCX);
    return condFor(e->op, isSignedInt(t), false);
}

void CodeGen::genBinary(BinaryExpr* e) {
    if (e->overload) {
        genCallTo(e->overload, {e->lhs, e->rhs});
        return;
    }
    switch (e->op) {
    case BinaryOp::LogAnd:
    case BinaryOp::LogOr: {
        Label falseL = as_.newLabel();
        Label endL = as_.newLabel();
        genBranch(e, falseL, false);
        as_.movRI(RAX, 1);
        as_.jmp(endL);
        as_.bind(falseL);
        as_.movRI(RAX, 0);
        as_.bind(endL);
        return;
    }
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge: {
        Cond c = genCompareFlags(e);
        as_.setcc(c, RAX);
        as_.movzx(RAX, RAX, 1);
        return;
    }
    default:
        break;
    }

    Type* t = e->type;
    if (t->isFloat()) {
        genOperandsFloat(e->lhs, e->rhs);
        bool f32 = t->size == 4;
        switch (e->op) {
        case BinaryOp::Add: as_.addS(XMM0, XMM1, f32); break;
        case BinaryOp::Sub: as_.subS(XMM0, XMM1, f32); break;
        case BinaryOp::Mul: as_.mulS(XMM0, XMM1, f32); break;
        case BinaryOp::Div: as_.divS(XMM0, XMM1, f32); break;
        default: break;
        }
        return;
    }

    Type* lt = e->lhs->type;
    if (lt->isPointer()) {
        genOperandsInt(e->lhs, e->rhs);
        uint32_t s = lt->elem->size;
        if (e->rhs->type->isPointer()) {   // ptr - ptr = element count
            as_.sub(RAX, RCX);
            if (s != 1) {
                as_.movRI(RCX, s);
                as_.cqo();
                as_.idiv(RCX);
            }
            return;
        }
        if (s != 1) as_.imulImm(RCX, RCX, static_cast<int32_t>(s));
        if (e->op == BinaryOp::Add) as_.add(RAX, RCX);
        else as_.sub(RAX, RCX);
        return;
    }

    genOperandsInt(e->lhs, e->rhs);
    emitIntOp(e->op, t);
}

void CodeGen::genUnary(UnaryExpr* e) {
    Type* t = e->type;
    switch (e->op) {
    case UnaryOp::Neg:
        if (e->overload) {
            genCallTo(e->overload, {e->operand});
            return;
        }
        genExpr(e->operand);
        if (t->isFloat()) {
            as_.movaps(XMM1, XMM0);
            as_.xorps(XMM0, XMM0);
            as_.subS(XMM0, XMM1, t->size == 4);
        } else {
            as_.neg(RAX);
            normalize(t);
        }
        return;
    case UnaryOp::Not:
        genExpr(e->operand);
        as_.xorImm8(RAX, 1);
        return;
    case UnaryOp::BitNot:
        genExpr(e->operand);
        as_.not_(RAX);
        normalize(t);
        return;
    case UnaryOp::AddrOf:
        genAddr(e->operand);
        return;
    case UnaryOp::Deref:
        genExpr(e->operand);
        loadValue(t, Mem::at(RAX, 0));
        return;
    default:
        genIncDec(e);
        return;
    }
}

void CodeGen::genIncDec(UnaryExpr* e) {
    Type* t = e->operand->type;
    bool inc = e->op == UnaryOp::PreInc || e->op == UnaryOp::PostInc;
    bool post = e->op == UnaryOp::PostInc || e->op == UnaryOp::PostDec;
    genAddr(e->operand);
    as_.movRR(R10, RAX);
    if (t->isFloat()) {
        bool f32 = t->size == 4;
        as_.movsLoad(XMM0, Mem::at(R10, 0), f32);
        as_.movsLoad(XMM1, Mem::ripRel(Section::Rdata, f32 ? addF32(1.0f) : addF64(1.0)), f32);
        if (post) as_.movaps(XMM2, XMM0);
        if (inc) as_.addS(XMM0, XMM1, f32);
        else as_.subS(XMM0, XMM1, f32);
        as_.movsStore(Mem::at(R10, 0), XMM0, f32);
        if (post) as_.movaps(XMM0, XMM2);
        return;
    }
    int32_t step = t->isPointer() ? static_cast<int32_t>(t->elem->size) : 1;
    as_.load(RAX, Mem::at(R10, 0), t->size, isSignedInt(t));
    as_.movRR(RCX, RAX);
    if (inc) as_.addImm(RAX, step);
    else as_.subImm(RAX, step);
    normalize(t);
    as_.store(Mem::at(R10, 0), RAX, t->size);
    if (post) as_.movRR(RAX, RCX);
}

void CodeGen::genAssign(AssignExpr* e) {
    Type* t = e->target->type;

    // Fast path: plain assignment to a local variable.
    if (!e->compound && e->target->kind == ExprKind::Ident) {
        auto* id = e->target->as<IdentExpr>();
        if (id->local && !id->local->indirect) {
            genExpr(e->value);
            storeValue(t, localMem(id->local));
            if (t->isAggregate()) as_.lea(RAX, localMem(id->local));
            return;
        }
    }

    genAddr(e->target);
    int32_t addrSlot = allocTemp8();
    as_.store(Mem::at(RBP, addrSlot), RAX, 8);
    genExpr(e->value);

    if (!e->compound) {
        as_.load(R10, Mem::at(RBP, addrSlot), 8, false);
        storeValue(t, Mem::at(R10, 0));
        if (t->isAggregate()) as_.movRR(RAX, R10);
        return;
    }

    if (t->isFloat()) {
        bool f32 = t->size == 4;
        as_.movaps(XMM1, XMM0);
        as_.load(R10, Mem::at(RBP, addrSlot), 8, false);
        as_.movsLoad(XMM0, Mem::at(R10, 0), f32);
        switch (e->op) {
        case BinaryOp::Add: as_.addS(XMM0, XMM1, f32); break;
        case BinaryOp::Sub: as_.subS(XMM0, XMM1, f32); break;
        case BinaryOp::Mul: as_.mulS(XMM0, XMM1, f32); break;
        case BinaryOp::Div: as_.divS(XMM0, XMM1, f32); break;
        default: break;
        }
        as_.movsStore(Mem::at(R10, 0), XMM0, f32);
        return;
    }

    as_.movRR(RCX, RAX);
    as_.load(R10, Mem::at(RBP, addrSlot), 8, false);
    as_.load(RAX, Mem::at(R10, 0), t->size, isSignedInt(t));
    if (t->isPointer()) {
        if (t->elem->size != 1) as_.imulImm(RCX, RCX, static_cast<int32_t>(t->elem->size));
        if (e->op == BinaryOp::Add) as_.add(RAX, RCX);
        else as_.sub(RAX, RCX);
    } else {
        emitIntOp(e->op, t);   // preserves R10 (div only clobbers RDX)
    }
    as_.store(Mem::at(R10, 0), RAX, t->size);
}

void CodeGen::genCallTo(FuncDecl* f, const std::vector<Expr*>& args) {
    Type* rt = f->returnType;
    bool hidden = passedByRef(rt);
    size_t n = args.size() + (hidden ? 1 : 0);
    maxOutgoing_ = std::max<int32_t>(maxOutgoing_, static_cast<int32_t>(8 * std::max<size_t>(4, n)));

    int32_t retSlot = hidden ? allocSlot(rt->size, std::max<uint32_t>(rt->align, 8)) : 0;

    // Evaluate every argument into a temporary first (later arguments may contain calls).
    std::vector<int32_t> slots(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        Type* pt = f->paramTypes[i];
        genExpr(args[i]);
        slots[i] = allocTemp8();
        Mem slot = Mem::at(RBP, slots[i]);
        if (pt->isFloat()) {
            as_.movsStore(slot, XMM0, pt->size == 4);
        } else if (pt->isAggregate()) {
            if (isRegSize(pt->size)) {
                as_.load(RAX, Mem::at(RAX, 0), pt->size, false);
            } else {
                // Large structs are passed as a pointer to a caller-owned copy.
                int32_t copy = allocSlot(pt->size, std::max<uint32_t>(pt->align, 8));
                copyMem(RBP, copy, RAX, 0, pt->size);
                as_.lea(RAX, Mem::at(RBP, copy));
            }
            as_.store(slot, RAX, 8);
        } else {
            as_.store(slot, RAX, 8);
        }
    }

    for (size_t k = 0; k < n; ++k) {
        if (hidden && k == 0) {
            as_.lea(RCX, Mem::at(RBP, retSlot));
            continue;
        }
        size_t i = k - (hidden ? 1 : 0);
        Type* pt = f->paramTypes[i];
        Mem slot = Mem::at(RBP, slots[i]);
        if (k < 4) {
            if (pt->isFloat()) as_.movsLoad(static_cast<Xmm>(k), slot, pt->size == 4);
            else as_.load(kArgRegs[k], slot, 8, false);
        } else {
            as_.load(RAX, slot, 8, false);
            as_.store(Mem::at(RSP, static_cast<int32_t>(8 * k)), RAX, 8);
        }
    }

    if (!f->externDll.empty()) as_.callMem(Mem::ripRel(Section::Import, importSlot(f->externDll, f->name)));
    else as_.call(funcLabels_[f]);

    if (rt->isVoid() || rt->isFloat()) return;
    if (hidden) {
        as_.lea(RAX, Mem::at(RBP, retSlot));
    } else if (rt->isAggregate()) {
        int32_t t = allocTemp8();
        as_.store(Mem::at(RBP, t), RAX, 8);
        as_.lea(RAX, Mem::at(RBP, t));
    } else {
        normalize(rt);
    }
}

void CodeGen::genCast(CastExpr* e) {
    Type* from = e->operand->type;
    Type* to = e->type;
    genExpr(e->operand);
    if (from == to) return;

    if (to->isFloat()) {
        if (from->isFloat()) {
            if (from->size == 4 && to->size == 8) as_.cvtss2sd(XMM0, XMM0);
            else if (from->size == 8 && to->size == 4) as_.cvtsd2ss(XMM0, XMM0);
        } else {
            as_.cvtsi2s(XMM0, RAX, to->size == 4);
        }
        return;
    }
    if (from->isFloat()) {
        if (to->isBool()) {
            as_.xorps(XMM1, XMM1);
            as_.ucomis(XMM0, XMM1, from->size == 4);
            as_.setcc(CC_NE, RAX);
            as_.movzx(RAX, RAX, 1);
        } else {
            as_.cvtts2si(RAX, XMM0, from->size == 4);
            normalize(to);
        }
        return;
    }
    if (to->isBool()) {
        as_.test(RAX, RAX);
        as_.setcc(CC_NE, RAX);
        as_.movzx(RAX, RAX, 1);
        return;
    }
    if (to->isInt()) normalize(to);
    // pointer <-> pointer, array -> pointer, null -> pointer: the bits are unchanged
}

void CodeGen::genStructLit(StructLitExpr* e) {
    Type* t = e->type;
    int32_t slot = allocSlot(t->size, std::max<uint32_t>(t->align, 8));
    zeroMem(RBP, slot, t->size);
    StructDecl* s = t->structDecl;
    for (size_t i = 0; i < e->fields.size(); ++i) {
        genExpr(e->fields[i]);
        storeValue(s->fields[i].type, Mem::at(RBP, slot + static_cast<int32_t>(s->fields[i].offset)));
    }
    as_.lea(RAX, Mem::at(RBP, slot));
}

} // namespace ron
