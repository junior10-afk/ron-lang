#include "codegen.hpp"

#include <algorithm>
#include <cstring>

namespace ron {

using namespace x64;

namespace {

constexpr Reg kArgRegs[4] = {RCX, RDX, R8, R9};
// Callee-saved registers available for local variables (Win64 ABI).
constexpr Reg kVarGprs[] = {RBX, RSI, RDI, R12, R13, R14, R15};
constexpr int kFirstVarXmm = 6;   // xmm6..xmm15 are callee-saved
constexpr int kLastVarXmm = 15;

int32_t alignUp(int32_t v, int32_t a) { return (v + a - 1) / a * a; }

bool isSignedInt(const Type* t) { return t->isInt() && t->isSigned; }

bool fitsInt32(uint64_t v) {
    auto s = static_cast<int64_t>(v);
    return s >= INT32_MIN && s <= INT32_MAX;
}

int scaleFor(uint32_t size) { return size == 1 || size == 2 || size == 4 || size == 8 ? static_cast<int>(size) : 0; }

// Strips implicit integer widenings: a widened register variable already holds
// the extended 64-bit value.
Expr* stripWidening(Expr* e) {
    while (e->kind == ExprKind::Cast && !e->as<CastExpr>()->target) {
        Expr* op = e->as<CastExpr>()->operand;
        if (!(op->type->isInt() || op->type->isBool()) || !e->type->isInt() || op->type->size > e->type->size ||
            (op->type->isSigned && !e->type->isSigned))
            break;
        e = op;
    }
    return e;
}

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
    debugInfoOffset_ = rdataAlloc(32, 8);   // filled by emitDebugInfo()
    genEntryStub();
    for (FuncDecl* f : sema_.functions) genFunction(f);
    for (auto& [index, f] : funcRefs_) as_.fixups[index].target = as_.labelPos(funcLabels_[f]);
    emitDebugInfo();
    image_.text = std::move(as_.code);
    image_.fixups = std::move(as_.fixups);
    return std::move(image_);
}

void CodeGen::genEntryStub() {
    image_.entryOffset = static_cast<uint32_t>(as_.pos());
    as_.subImm(RSP, 40);   // shadow space + realign (entry rsp is 8 mod 16)
    if (sema_.runtimeInit) as_.call(funcLabels_[sema_.runtimeInit]);     // crash handler
    if (sema_.initFunction) as_.call(funcLabels_[sema_.initFunction]);   // globals with computed values
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

// ───────────────────────────── Register allocation ─────────────────────────────

// Scalar variables whose address is never taken live in callee-saved registers
// for the whole function, most frequently used (loop-weighted) first.
void CodeGen::allocateRegisters(FuncDecl* f) {
    std::vector<LocalVar*> candidates;
    auto consider = [&](LocalVar* v) {
        v->reg = -1;
        if (!v->type || v->addressTaken || passedByRef(v->type)) return;
        if (v->type->isScalar() || v->type->isFloat()) candidates.push_back(v);
    };
    for (LocalVar* v : f->paramVars) consider(v);
    for (LocalVar* v : f->locals) consider(v);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](LocalVar* a, LocalVar* b) { return a->useWeight > b->useWeight; });

    size_t nextGpr = 0;
    int nextXmm = kFirstVarXmm;
    for (LocalVar* v : candidates) {
        if (v->type->isFloat()) {
            if (nextXmm <= kLastVarXmm) v->reg = static_cast<int8_t>(nextXmm++);
        } else if (nextGpr < std::size(kVarGprs)) {
            v->reg = static_cast<int8_t>(kVarGprs[nextGpr++]);
        }
    }
    for (size_t i = 0; i < nextGpr; ++i) savedGprs_.push_back({kVarGprs[i], 0});
    for (int x = kFirstVarXmm; x < nextXmm; ++x) savedXmms_.push_back({static_cast<Xmm>(x), 0});
}

int CodeGen::regOf(Expr* e) const {
    e = stripWidening(e);
    if (e->kind != ExprKind::Ident) return -1;
    LocalVar* v = e->as<IdentExpr>()->local;
    return v && v->reg >= 0 && !v->type->isFloat() ? v->reg : -1;
}

int CodeGen::xmmOf(Expr* e) const {
    if (e->kind != ExprKind::Ident) return -1;
    LocalVar* v = e->as<IdentExpr>()->local;
    return v && v->reg >= 0 && v->type->isFloat() ? v->reg : -1;
}

bool CodeGen::isSimple(Expr* e) const {
    if (e->kind == ExprKind::IntLit || e->kind == ExprKind::NullLit || e->kind == ExprKind::BoolLit) return true;
    Expr* x = stripWidening(e);
    if (x->kind != ExprKind::Ident || !x->type->isScalar()) return false;
    auto* id = x->as<IdentExpr>();
    return (id->local && !id->local->indirect) || id->global;
}

bool CodeGen::isSimpleFloat(Expr* e) const {
    if (e->kind == ExprKind::FloatLit) return true;
    if (e->kind != ExprKind::Ident || !e->type->isFloat()) return false;
    auto* id = e->as<IdentExpr>();
    return (id->local && !id->local->indirect) || id->global;
}

// ───────────────────────────── Functions ─────────────────────────────

void CodeGen::genFunction(FuncDecl* f) {
    func_ = f;
    frameSize_ = 0;
    maxFrame_ = 0;
    maxOutgoing_ = 32;
    loops_.clear();
    deferFrames_.clear();
    savedGprs_.clear();
    savedXmms_.clear();
    hiddenReturn_ = passedByRef(f->returnType);
    allocateRegisters(f);

    as_.bind(funcLabels_[f]);
    uint32_t funcStart = static_cast<uint32_t>(as_.pos());
    as_.push(RBP);
    as_.movRR(RBP, RSP);
    uint32_t subAt = static_cast<uint32_t>(as_.pos());
    uint32_t frameImm = 0;
    as_.subImm32Patchable(RSP, frameImm);   // 7 bytes, patched once the frame size is known

    // Save the callee-saved registers we use.
    for (auto& [r, slot] : savedGprs_) {
        slot = allocSlot(8, 8);
        as_.store(Mem::at(RBP, slot), r, 8);
    }
    for (auto& [x, slot] : savedXmms_) {
        slot = allocSlot(16, 16);
        as_.movupsStore(Mem::at(RBP, slot), x);
    }

    // Parameters: into their register, or homed into their shadow-space slot.
    int k0 = hiddenReturn_ ? 1 : 0;
    if (hiddenReturn_) as_.store(Mem::at(RBP, 16), RCX, 8);
    for (size_t i = 0; i < f->paramVars.size(); ++i) {
        int k = static_cast<int>(i) + k0;
        LocalVar* v = f->paramVars[i];
        v->frameOffset = 16 + 8 * k;
        v->indirect = passedByRef(v->type);
        bool isFloat = v->type->isFloat();
        bool f32 = v->type->size == 4;
        if (v->reg >= 0) {
            if (isFloat) {
                if (k < 4) as_.movaps(static_cast<Xmm>(v->reg), static_cast<Xmm>(k));
                else as_.movsLoad(static_cast<Xmm>(v->reg), localMem(v), f32);
            } else {
                Reg r = static_cast<Reg>(v->reg);
                if (k < 4) as_.movRR(r, kArgRegs[k]);
                else as_.load(r, localMem(v), 8, false);
                normalize(v->type, r);
            }
        } else if (k < 4) {
            if (isFloat) as_.movsStore(localMem(v), static_cast<Xmm>(k), f32);
            else as_.store(localMem(v), kArgRegs[k], 8);
        }
    }

    epilogue_ = as_.newLabel();
    genStmt(f->body);
    as_.movRI(RAX, 0);   // falling off the end returns 0 (main's implicit exit code)
    as_.bind(epilogue_);
    for (auto& [r, slot] : savedGprs_) as_.load(r, Mem::at(RBP, slot), 8, false);
    for (auto& [x, slot] : savedXmms_) as_.movupsLoad(x, Mem::at(RBP, slot));
    as_.movRR(RSP, RBP);
    as_.pop(RBP);
    as_.ret();

    int32_t frame = alignUp(maxFrame_, 16) + alignUp(maxOutgoing_, 16);
    if (frame <= 4096) {
        as_.patch32(frameImm, static_cast<uint32_t>(frame));
        funcRanges_.push_back({funcStart, static_cast<uint32_t>(as_.pos()), f->name, f->loc.fileId});
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
    funcRanges_.push_back({funcStart, static_cast<uint32_t>(as_.pos()), f->name, f->loc.fileId});
}

// ───────────────────────────── Statements ─────────────────────────────

void CodeGen::genStmt(Stmt* s) {
    if (s->loc.fileId != UINT32_MAX && s->loc.line > 0 &&
        (lineMarks_.empty() || lineMarks_.back().line != s->loc.line || lineMarks_.back().fileId != s->loc.fileId)) {
        auto pos = static_cast<uint32_t>(as_.pos());
        if (!lineMarks_.empty() && lineMarks_.back().pos == pos) lineMarks_.back() = {pos, s->loc.fileId, s->loc.line};
        else lineMarks_.push_back({pos, s->loc.fileId, s->loc.line});
    }
    switch (s->kind) {
    case StmtKind::Block:
        genScope(s->as<BlockStmt>()->stmts);
        break;
    case StmtKind::ForIn:
        genStmt(s->as<ForInStmt>()->lowered);
        break;
    case StmtKind::Defer:
        // Nothing runs now: the statement is replayed on every exit of the enclosing block.
        if (!deferFrames_.empty()) deferFrames_.back().push_back(s->as<DeferStmt>()->body);
        break;
    case StmtKind::Switch: {
        auto* sw = s->as<SwitchStmt>();
        int32_t mark = frameSize_;
        int vr = regOf(sw->subject);
        Reg subject = RAX;
        if (vr >= 0) subject = static_cast<Reg>(vr);
        else genExpr(sw->subject);
        // Compare-and-branch chain to each case
        Label end = as_.newLabel();
        Label defaultLabel = end;
        std::vector<Label> caseLabels;
        for (const SwitchCase& c : sw->cases) {
            Label l = as_.newLabel();
            caseLabels.push_back(l);
            if (c.isDefault) defaultLabel = l;
            for (Expr* v : c.values) {
                uint64_t value = v->as<IntLitExpr>()->value;
                if (fitsInt32(value)) {
                    as_.cmpImm(subject, static_cast<int32_t>(value));
                } else {
                    as_.movRI(RCX, value);
                    as_.cmp(subject, RCX);
                }
                as_.jcc(CC_E, l);
            }
        }
        as_.jmp(defaultLabel);
        frameSize_ = mark;
        loops_.push_back({end, 0, "", deferFrames_.size(), true});
        for (size_t i = 0; i < sw->cases.size(); ++i) {
            as_.bind(caseLabels[i]);
            genScope(sw->cases[i].body);
            as_.jmp(end);
        }
        loops_.pop_back();
        as_.bind(end);
        break;
    }
    case StmtKind::DoWhile: {
        auto* d = s->as<DoWhileStmt>();
        Label body = as_.newLabel();
        Label cond = as_.newLabel();
        Label end = as_.newLabel();
        as_.bind(body);
        loops_.push_back({end, cond, d->label, deferFrames_.size(), false});
        genStmt(d->body);
        loops_.pop_back();
        as_.bind(cond);
        int32_t mark = frameSize_;
        genBranch(d->cond, body, true);
        frameSize_ = mark;
        as_.bind(end);
        break;
    }
    case StmtKind::VarDecl: {
        auto* v = s->as<VarDeclStmt>();
        LocalVar* var = v->var;
        if (var->reg >= 0) {
            int32_t mark = frameSize_;
            bool isFloat = var->type->isFloat();
            if (v->init) {
                genExpr(v->init);
                if (isFloat) as_.movaps(static_cast<Xmm>(var->reg), XMM0);
                else as_.movRR(static_cast<Reg>(var->reg), RAX);
            } else if (isFloat) {
                as_.xorps(static_cast<Xmm>(var->reg), static_cast<Xmm>(var->reg));
            } else {
                as_.movRI(static_cast<Reg>(var->reg), 0);
            }
            frameSize_ = mark;
            break;
        }
        var->frameOffset = allocSlot(var->type->size, var->type->align);
        int32_t mark = frameSize_;
        if (v->init) {
            genExpr(v->init);
            storeValue(var->type, localMem(var));
        } else {
            zeroMem(localMem(var), var->type->size);
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
        // Loop rotation: test at the bottom, one jump per iteration.
        auto* w = s->as<WhileStmt>();
        Label body = as_.newLabel();
        Label cond = as_.newLabel();
        Label end = as_.newLabel();
        as_.jmp(cond);
        as_.align(16);   // hot loop entry
        as_.bind(body);
        loops_.push_back({end, cond, w->label, deferFrames_.size(), false});
        genStmt(w->body);
        loops_.pop_back();
        as_.bind(cond);
        int32_t mark = frameSize_;
        genBranch(w->cond, body, true);
        frameSize_ = mark;
        as_.bind(end);
        break;
    }
    case StmtKind::For: {
        auto* f = s->as<ForStmt>();
        if (f->init) genStmt(f->init);
        Label body = as_.newLabel();
        Label cont = as_.newLabel();
        Label cond = as_.newLabel();
        Label end = as_.newLabel();
        as_.jmp(cond);
        as_.align(16);   // hot loop entry
        as_.bind(body);
        loops_.push_back({end, cont, f->label, deferFrames_.size(), false});
        genStmt(f->body);
        loops_.pop_back();
        as_.bind(cont);
        int32_t mark = frameSize_;
        if (f->step) genExpr(f->step);
        frameSize_ = mark;
        as_.bind(cond);
        if (f->cond) genBranch(f->cond, body, true);
        else as_.jmp(body);
        frameSize_ = mark;
        as_.bind(end);
        break;
    }
    case StmtKind::Return: {
        auto* r = s->as<ReturnStmt>();
        int32_t mark = frameSize_;
        if (r->value) genExpr(r->value);
        else if (func_->returnType->isInt()) as_.movRI(RAX, 0);
        finishReturn(r->value != nullptr);
        frameSize_ = mark;
        break;
    }
    case StmtKind::Match:
        genMatch(s->as<MatchStmt>());
        break;
    case StmtKind::Break: {
        int i = findLoop(s->as<BreakStmt>()->label, false);
        emitDefers(loops_[i].deferDepth);
        as_.jmp(loops_[i].breakLabel);
        break;
    }
    case StmtKind::Continue: {
        int i = findLoop(s->as<ContinueStmt>()->label, true);
        emitDefers(loops_[i].deferDepth);
        as_.jmp(loops_[i].continueLabel);
        break;
    }
    }
}

// The return value is in RAX / XMM0 (aggregates: their address in RAX).
void CodeGen::finishReturn(bool hasValue) {
    Type* rt = func_->returnType;
    if (hasValue && rt->isAggregate()) {
        if (hiddenReturn_) {
            as_.load(R10, Mem::at(RBP, 16), 8, false);
            copyMem(Mem::at(R10, 0), Mem::at(RAX, 0), rt->size);
            as_.movRR(RAX, R10);
        } else {
            as_.load(RAX, Mem::at(RAX, 0), rt->size, false);
        }
    }
    bool pending = false;
    for (const auto& frame : deferFrames_) pending |= !frame.empty();
    if (pending) {
        // Keep the return value safe while the deferred statements run.
        int32_t mark = frameSize_;
        int32_t save = allocTemp8();
        bool isFloat = rt->isFloat();
        if (isFloat) as_.movsStore(Mem::at(RBP, save), XMM0, false);
        else as_.store(Mem::at(RBP, save), RAX, 8);
        emitDefers(0);
        if (isFloat) as_.movsLoad(XMM0, Mem::at(RBP, save), false);
        else as_.load(RAX, Mem::at(RBP, save), 8, false);
        frameSize_ = mark;
    }
    as_.jmp(epilogue_);
}

static int variantIndex(EnumDecl* e, const char* name) {
    for (size_t i = 0; i < e->members.size(); ++i)
        if (e->members[i].name == name) return static_cast<int>(i);
    return 0;
}

void CodeGen::genVariant(VariantExpr* v) {
    EnumDecl* e = v->sum;
    Type* t = e->type;
    int32_t slot = allocSlot(t->size, std::max<uint32_t>(t->align, 8));
    zeroMem(Mem::at(RBP, slot), t->size);
    as_.storeImm32(Mem::at(RBP, slot), v->member, 4);
    const EnumMember& m = e->members[v->member];
    for (size_t i = 0; i < v->args.size(); ++i) {
        genExpr(v->args[i]);
        storeValue(m.payloadTypes[i], Mem::at(RBP, slot + static_cast<int32_t>(e->payloadOffset + m.payloadOffsets[i])));
    }
    as_.lea(RAX, Mem::at(RBP, slot));
}

// value? → the success payload, or an early return of None / Err(e)
void CodeGen::genTry(TryExpr* e) {
    EnumDecl* sum = e->operand->type->sumDecl;
    bool isResult = sum->genericOrigin->name == "Result";
    int success = variantIndex(sum, isResult ? "Ok" : "Some");
    genExpr(e->operand);
    int32_t ptrSlot = allocTemp8();
    as_.store(Mem::at(RBP, ptrSlot), RAX, 8);
    as_.load(RCX, Mem::at(RAX, 0), 4, false);
    as_.cmpImm(RCX, success);
    Label ok = as_.newLabel();
    as_.jcc(CC_E, ok);

    // Failure: build the function's own None / Err(value) and return it.
    Type* rt = func_->returnType;
    EnumDecl* out = rt->sumDecl;
    int32_t slot = allocSlot(rt->size, std::max<uint32_t>(rt->align, 8));
    zeroMem(Mem::at(RBP, slot), rt->size);
    int failure = variantIndex(out, isResult ? "Err" : "None");
    as_.storeImm32(Mem::at(RBP, slot), failure, 4);
    if (isResult) {
        int err = variantIndex(sum, "Err");
        const EnumMember& src = sum->members[err];
        const EnumMember& dst = out->members[failure];
        if (!src.payloadTypes.empty() && !dst.payloadTypes.empty()) {
            as_.load(R10, Mem::at(RBP, ptrSlot), 8, false);
            copyMem(Mem::at(RBP, slot + static_cast<int32_t>(out->payloadOffset + dst.payloadOffsets[0])),
                    Mem::at(R10, static_cast<int32_t>(sum->payloadOffset + src.payloadOffsets[0])),
                    src.payloadTypes[0]->size);
        }
    }
    as_.lea(RAX, Mem::at(RBP, slot));
    finishReturn(true);

    as_.bind(ok);
    as_.load(RAX, Mem::at(RBP, ptrSlot), 8, false);
    const EnumMember& m = sum->members[success];
    loadValue(m.payloadTypes[0], Mem::at(RAX, static_cast<int32_t>(sum->payloadOffset + m.payloadOffsets[0])));
}

void CodeGen::genMatch(MatchStmt* m) {
    Type* t = m->subject->type;
    EnumDecl* sum = t->sumDecl;
    int32_t mark = frameSize_;
    int32_t ptrSlot = 0;
    if (sum) {
        genExpr(m->subject);   // address of the tagged union
        ptrSlot = allocTemp8();
        as_.store(Mem::at(RBP, ptrSlot), RAX, 8);
        as_.load(RCX, Mem::at(RAX, 0), 4, false);
    } else {
        genExpr(m->subject);   // plain enum value
        as_.movRR(RCX, RAX);
    }
    Label end = as_.newLabel();
    Label fallback = end;
    std::vector<Label> labels;
    for (const MatchArm& arm : m->arms) {
        Label l = as_.newLabel();
        labels.push_back(l);
        if (arm.member < 0) {
            fallback = l;
            continue;
        }
        int64_t value = sum ? arm.member : t->enumDecl->members[arm.member].resolved;
        as_.cmpImm(RCX, static_cast<int32_t>(value));
        as_.jcc(CC_E, l);
    }
    as_.jmp(fallback);
    for (size_t i = 0; i < m->arms.size(); ++i) {
        const MatchArm& arm = m->arms[i];
        as_.bind(labels[i]);
        if (sum && arm.member >= 0) {
            const EnumMember& mem = sum->members[arm.member];
            for (size_t b = 0; b < arm.vars.size(); ++b) {
                LocalVar* v = arm.vars[b];
                if (!v) continue;
                as_.load(R10, Mem::at(RBP, ptrSlot), 8, false);
                loadValue(v->type, Mem::at(R10, static_cast<int32_t>(sum->payloadOffset + mem.payloadOffsets[b])));
                if (v->reg >= 0) {
                    if (v->type->isFloat()) as_.movaps(static_cast<Xmm>(v->reg), XMM0);
                    else as_.movRR(static_cast<Reg>(v->reg), RAX);
                } else {
                    v->frameOffset = allocSlot(v->type->size, v->type->align);
                    storeValue(v->type, localMem(v));
                }
            }
        }
        genStmt(arm.body);
        as_.jmp(end);
    }
    as_.bind(end);
    frameSize_ = mark;
}

void CodeGen::genScope(const std::vector<Stmt*>& stmts) {
    deferFrames_.emplace_back();
    for (Stmt* c : stmts) genStmt(c);
    // Normal exit: run this block's defers in reverse order.
    std::vector<Stmt*> pending = deferFrames_.back();
    deferFrames_.pop_back();
    for (auto it = pending.rbegin(); it != pending.rend(); ++it) genStmt(*it);
}

void CodeGen::emitDefers(size_t downTo) {
    // Copy first: generating a deferred statement may push frames of its own.
    std::vector<Stmt*> toRun;
    for (size_t f = deferFrames_.size(); f > downTo; --f)
        for (auto it = deferFrames_[f - 1].rbegin(); it != deferFrames_[f - 1].rend(); ++it) toRun.push_back(*it);
    for (Stmt* s : toRun) genStmt(s);
}

int CodeGen::findLoop(const std::string& label, bool isContinue) const {
    for (int i = static_cast<int>(loops_.size()) - 1; i >= 0; --i) {
        const Loop& l = loops_[i];
        if (!label.empty()) {
            if (!l.isSwitch && l.label == label) return i;
        } else if (!isContinue || !l.isSwitch) {
            return i;
        }
    }
    return static_cast<int>(loops_.size()) - 1;   // sema guarantees a target exists
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
    int r = regOf(cond);
    if (r >= 0) {
        as_.test(static_cast<Reg>(r), static_cast<Reg>(r));
    } else {
        genExpr(cond);
        as_.test(RAX, RAX);
    }
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
    if (t->isFloat()) as_.movsStore(m, XMM0, t->size == 4);
    else if (t->isAggregate()) copyMem(m, Mem::at(RAX, 0), t->size);
    else as_.store(m, RAX, t->size);
}

void CodeGen::copyMem(const Mem& dstIn, const Mem& srcIn, uint32_t size) {
    Mem dst = dstIn, src = srcIn;
    // Indexed or RIP-relative operands: materialize the addresses first.
    if (src.rip || src.hasIndex()) {
        as_.lea(R11, src);
        src = Mem::at(R11, 0);
    }
    if (dst.rip || dst.hasIndex()) {
        as_.lea(R10, dst);
        dst = Mem::at(R10, 0);
    }
    if (size <= 128) {
        Reg data = src.base == R11 || dst.base == R11 ? RDX : R11;
        uint32_t i = 0;
        for (unsigned chunk : {8u, 4u, 2u, 1u}) {
            for (; i + chunk <= size; i += chunk) {
                as_.load(data, Mem::at(src.base, src.disp + static_cast<int32_t>(i)), chunk, false);
                as_.store(Mem::at(dst.base, dst.disp + static_cast<int32_t>(i)), data, chunk);
            }
        }
        return;
    }
    as_.lea(R11, src);
    as_.lea(R10, dst);
    as_.push(RSI);
    as_.push(RDI);
    as_.movRR(RSI, R11);
    as_.movRR(RDI, R10);
    as_.movRI(RCX, size);
    as_.repMovsb();
    as_.pop(RDI);
    as_.pop(RSI);
}

void CodeGen::zeroMem(const Mem& dstIn, uint32_t size) {
    Mem dst = dstIn;
    if (dst.rip || dst.hasIndex()) {
        as_.lea(R10, dst);
        dst = Mem::at(R10, 0);
    }
    if (size <= 128) {
        as_.movRI(R11, 0);
        uint32_t i = 0;
        for (unsigned chunk : {8u, 4u, 2u, 1u})
            for (; i + chunk <= size; i += chunk)
                as_.store(Mem::at(dst.base, dst.disp + static_cast<int32_t>(i)), R11, chunk);
        return;
    }
    as_.lea(R11, dst);
    as_.push(RDI);
    as_.movRR(RDI, R11);
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
    int vr = regOf(e);
    if (vr >= 0) {
        as_.movRR(r, static_cast<Reg>(vr));
        return true;
    }
    Expr* x = stripWidening(e);
    if (x->kind != ExprKind::Ident || !x->type->isScalar()) return false;
    auto* id = x->as<IdentExpr>();
    if (id->local && !id->local->indirect) {
        as_.load(r, localMem(id->local), x->type->size, isSignedInt(x->type));
        return true;
    }
    if (id->global) {
        as_.load(r, Mem::ripRel(Section::Data, id->global->dataOffset), x->type->size, isSignedInt(x->type));
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
    int vx = xmmOf(e);
    if (vx >= 0) {
        as_.movaps(x, static_cast<Xmm>(vx));
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

std::pair<Reg, Reg> CodeGen::genOperandsInt(Expr* lhs, Expr* rhs, bool lhsInAcc) {
    int l = regOf(lhs);
    int r = regOf(rhs);
    if (!lhsInAcc && l >= 0) {
        if (r >= 0) return {static_cast<Reg>(l), static_cast<Reg>(r)};
        if (isSimple(rhs)) {
            simpleInto(rhs, RCX);
            return {static_cast<Reg>(l), RCX};
        }
        genExpr(rhs);
        return {static_cast<Reg>(l), RAX};
    }
    if (r >= 0) {
        genExpr(lhs);
        return {RAX, static_cast<Reg>(r)};
    }
    if (isSimple(rhs)) {
        genExpr(lhs);
        simpleInto(rhs, RCX);
        return {RAX, RCX};
    }
    if (l >= 0) {
        genExpr(rhs);
        as_.movRR(RCX, RAX);
        as_.movRR(RAX, static_cast<Reg>(l));
        return {RAX, RCX};
    }
    genExpr(lhs);
    int32_t t = allocTemp8();
    as_.store(Mem::at(RBP, t), RAX, 8);
    genExpr(rhs);
    as_.movRR(RCX, RAX);
    as_.load(RAX, Mem::at(RBP, t), 8, false);
    return {RAX, RCX};
}

std::pair<Xmm, Xmm> CodeGen::genOperandsFloat(Expr* lhs, Expr* rhs, bool lhsInAcc) {
    int l = xmmOf(lhs);
    int r = xmmOf(rhs);
    bool f32 = lhs->type->size == 4;
    if (!lhsInAcc && l >= 0) {
        if (r >= 0) return {static_cast<Xmm>(l), static_cast<Xmm>(r)};
        if (isSimpleFloat(rhs)) {
            simpleFloatInto(rhs, XMM1);
            return {static_cast<Xmm>(l), XMM1};
        }
        genExpr(rhs);
        return {static_cast<Xmm>(l), XMM0};
    }
    if (r >= 0) {
        genExpr(lhs);
        return {XMM0, static_cast<Xmm>(r)};
    }
    if (isSimpleFloat(rhs)) {
        genExpr(lhs);
        simpleFloatInto(rhs, XMM1);
        return {XMM0, XMM1};
    }
    if (l >= 0) {
        genExpr(rhs);
        as_.movaps(XMM1, XMM0);
        as_.movaps(XMM0, static_cast<Xmm>(l));
        return {XMM0, XMM1};
    }
    genExpr(lhs);
    int32_t t = allocTemp8();
    as_.movsStore(Mem::at(RBP, t), XMM0, f32);
    genExpr(rhs);
    as_.movaps(XMM1, XMM0);
    as_.movsLoad(XMM0, Mem::at(RBP, t), f32);
    return {XMM0, XMM1};
}

void CodeGen::emitIntOp(BinaryOp op, Type* t, Reg rhs) {
    bool s = isSignedInt(t);
    switch (op) {
    case BinaryOp::Add: as_.add(RAX, rhs); break;
    case BinaryOp::Sub: as_.sub(RAX, rhs); break;
    case BinaryOp::Mul: as_.imul(RAX, rhs); break;
    case BinaryOp::Div:
    case BinaryOp::Mod:
        if (s) {
            as_.cqo();
            as_.idiv(rhs);
        } else {
            as_.movRI(RDX, 0);
            as_.div(rhs);
        }
        if (op == BinaryOp::Mod) as_.movRR(RAX, RDX);
        break;
    case BinaryOp::BitAnd: as_.and_(RAX, rhs); break;
    case BinaryOp::BitOr: as_.or_(RAX, rhs); break;
    case BinaryOp::BitXor: as_.xor_(RAX, rhs); break;
    case BinaryOp::Shl:
    case BinaryOp::Shr:
        if (rhs != RCX) as_.movRR(RCX, rhs);
        if (op == BinaryOp::Shl) as_.shlCl(RAX);
        else if (s) as_.sarCl(RAX);
        else as_.shrCl(RAX);
        break;
    default: break;
    }
    normalize(t);
}

// dst op= literal, using immediate instruction forms. Returns false if not applicable.
bool CodeGen::emitIntOpImm(BinaryOp op, Type* t, Reg dst, Expr* rhs) {
    if (rhs->kind != ExprKind::IntLit) return false;
    uint64_t v = rhs->as<IntLitExpr>()->value;
    bool s = isSignedInt(t);
    switch (op) {
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::BitAnd:
    case BinaryOp::BitOr:
    case BinaryOp::BitXor:
    case BinaryOp::Mul:
        if (!fitsInt32(v)) return false;
        break;
    case BinaryOp::Shl:
    case BinaryOp::Shr:
        if (v >= 64) return false;
        break;
    default:
        return false;
    }
    auto imm = static_cast<int32_t>(v);
    switch (op) {
    case BinaryOp::Add: as_.addImm(dst, imm); break;
    case BinaryOp::Sub: as_.subImm(dst, imm); break;
    case BinaryOp::BitAnd: as_.andImm(dst, imm); break;
    case BinaryOp::BitOr: as_.orImm(dst, imm); break;
    case BinaryOp::BitXor: as_.xorImm(dst, imm); break;
    case BinaryOp::Mul: as_.imulImm(dst, dst, imm); break;
    case BinaryOp::Shl: as_.shlImm(dst, static_cast<uint8_t>(v)); break;
    case BinaryOp::Shr:
        if (s) as_.sarImm(dst, static_cast<uint8_t>(v));
        else as_.shrImm(dst, static_cast<uint8_t>(v));
        break;
    default: break;
    }
    normalize(t, dst);
    return true;
}

void CodeGen::emitFloatOp(BinaryOp op, bool f32, Xmm dst, Xmm src) {
    switch (op) {
    case BinaryOp::Add: as_.addS(dst, src, f32); break;
    case BinaryOp::Sub: as_.subS(dst, src, f32); break;
    case BinaryOp::Mul: as_.mulS(dst, src, f32); break;
    case BinaryOp::Div: as_.divS(dst, src, f32); break;
    default: break;
    }
}

// ───────────────────────────── Memory operands ─────────────────────────────

bool CodeGen::stableMem(Expr* e, Mem& out) {
    switch (e->kind) {
    case ExprKind::Ident: {
        auto* id = e->as<IdentExpr>();
        if (id->local) {
            if (id->local->reg >= 0 || id->local->indirect) return false;
            out = localMem(id->local);
            return true;
        }
        if (!id->global) return false;
        out = Mem::ripRel(Section::Data, id->global->dataOffset);
        return true;
    }
    case ExprKind::Unary: {
        auto* u = e->as<UnaryExpr>();
        int r = u->op == UnaryOp::Deref ? regOf(u->operand) : -1;
        if (r < 0) return false;
        out = Mem::at(static_cast<Reg>(r), 0);
        return true;
    }
    case ExprKind::Index: {
        auto* ix = e->as<IndexExpr>();
        Type* bt = ix->base->type;
        uint32_t s = bt->elem->size;
        // Checked indexes go through genIndexMem, which emits the bounds check.
        if (boundsChecks && (bt->isArray() || bt->isSlice()) && ix->index->kind != ExprKind::IntLit) return false;
        Mem base;
        if (bt->isArray()) {
            if (!stableMem(ix->base, base) || base.hasIndex()) return false;
        } else {
            int r = regOf(ix->base);
            if (r < 0) return false;
            base = Mem::at(static_cast<Reg>(r), 0);
        }
        if (ix->index->kind == ExprKind::IntLit) {
            int64_t off = static_cast<int64_t>(ix->index->as<IntLitExpr>()->value) * s;
            if (off < INT32_MIN || off > INT32_MAX) return false;
            out = base;
            addDisp(out, static_cast<int32_t>(off));
            return true;
        }
        int r = regOf(ix->index);
        if (r < 0 || !scaleFor(s) || base.rip) return false;
        out = Mem::indexed(base.base, static_cast<Reg>(r), static_cast<uint8_t>(s), base.disp);
        return true;
    }
    case ExprKind::Member: {
        auto* m = e->as<MemberExpr>();
        if (m->throughPointer) {
            int r = regOf(m->base);
            if (r < 0) return false;
            out = Mem::at(static_cast<Reg>(r), static_cast<int32_t>(m->offset));
            return true;
        }
        if (!stableMem(m->base, out)) return false;
        addDisp(out, static_cast<int32_t>(m->offset));
        return true;
    }
    default:
        return false;
    }
}

Mem CodeGen::genIndexMem(IndexExpr* e) {
    Type* bt = e->base->type;
    uint32_t s = bt->elem->size;
    Mem base;
    bool baseInRax = false;
    bool check = boundsChecks && (bt->isArray() || bt->isSlice());
    int32_t lenSlot = 0;
    if (bt->isSlice()) {
        Mem sm = genMem(e->base);
        if (check) {
            Mem lm = sm;
            addDisp(lm, 8);
            as_.load(R11, lm, 8, false);   // slice.len, kept for the bounds check
            lenSlot = allocTemp8();
            as_.store(Mem::at(RBP, lenSlot), R11, 8);
        }
        as_.load(RAX, sm, 8, false);   // slice.ptr
        base = Mem::at(RAX, 0);
        baseInRax = true;
    } else if (bt->isArray()) {
        if (stableMem(e->base, base) && !base.hasIndex() && !base.rip) {
            // [rbp + offset] or [reg + offset]: no code needed
        } else {
            base = genMem(e->base);
            if (base.hasIndex() || base.rip || base.base != RAX) {
                as_.lea(RAX, base);
                base = Mem::at(RAX, 0);
            }
            baseInRax = true;
        }
    } else {
        int r = regOf(e->base);
        if (r >= 0) {
            base = Mem::at(static_cast<Reg>(r), 0);
        } else {
            genExpr(e->base);
            base = Mem::at(RAX, 0);
            baseInRax = true;
        }
    }

    auto checkIndex = [&](Reg idx) {
        if (!check) return;
        if (bt->isArray()) boundsCheck(idx, true, bt->count, 0, false, e->loc);
        else boundsCheck(idx, false, 0, lenSlot, false, e->loc);
    };
    if (e->index->kind == ExprKind::IntLit) {
        int64_t off = static_cast<int64_t>(e->index->as<IntLitExpr>()->value) * s;
        if (off >= INT32_MIN && off <= INT32_MAX) {
            if (check && bt->isSlice()) {   // arrays are checked at compile time
                as_.movRI(RCX, e->index->as<IntLitExpr>()->value);
                checkIndex(RCX);
            }
            base.disp += static_cast<int32_t>(off);
            return base;
        }
    }
    int ir = regOf(e->index);
    if (ir >= 0 && scaleFor(s)) {
        checkIndex(static_cast<Reg>(ir));
        return Mem::indexed(base.base, static_cast<Reg>(ir), static_cast<uint8_t>(s), base.disp);
    }

    // Compute the index into RCX (keeping the base alive).
    if (ir >= 0) {
        as_.movRR(RCX, static_cast<Reg>(ir));
    } else if (isSimple(e->index)) {
        simpleInto(e->index, RCX);
    } else if (baseInRax) {
        int32_t t = allocTemp8();
        as_.store(Mem::at(RBP, t), RAX, 8);
        genExpr(e->index);
        as_.movRR(RCX, RAX);
        as_.load(RAX, Mem::at(RBP, t), 8, false);
    } else {
        genExpr(e->index);
        as_.movRR(RCX, RAX);
    }
    checkIndex(RCX);
    if (scaleFor(s)) return Mem::indexed(base.base, RCX, static_cast<uint8_t>(s), base.disp);
    as_.imulImm(RCX, RCX, static_cast<int32_t>(s));
    return Mem::indexed(base.base, RCX, 1, base.disp);
}

Mem CodeGen::genMem(Expr* e) {
    Mem m;
    if (stableMem(e, m)) return m;
    switch (e->kind) {
    case ExprKind::Ident: {
        auto* id = e->as<IdentExpr>();
        as_.load(RAX, localMem(id->local), 8, false);   // indirect (large struct parameter)
        return Mem::at(RAX, 0);
    }
    case ExprKind::Unary:
        if (e->as<UnaryExpr>()->op == UnaryOp::Deref) {
            genExpr(e->as<UnaryExpr>()->operand);
            return Mem::at(RAX, 0);
        }
        break;
    case ExprKind::Index:
        return genIndexMem(e->as<IndexExpr>());
    case ExprKind::Member: {
        auto* mem = e->as<MemberExpr>();
        if (mem->throughPointer) {
            genExpr(mem->base);
            return Mem::at(RAX, static_cast<int32_t>(mem->offset));
        }
        m = genMem(mem->base);
        addDisp(m, static_cast<int32_t>(mem->offset));
        return m;
    }
    default:
        break;
    }
    genExpr(e);   // aggregate rvalues evaluate to the address of a temporary
    return Mem::at(RAX, 0);
}

void CodeGen::genAddr(Expr* e) {
    Mem m = genMem(e);
    if (!(m.base == RAX && m.disp == 0 && !m.hasIndex() && !m.rip)) as_.lea(RAX, m);
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
        LocalVar* v = e->as<IdentExpr>()->local;
        if (FuncDecl* fn = e->as<IdentExpr>()->func) {
            genFunctionAddress(fn);
            break;
        }
        if (v && v->reg >= 0) {
            if (v->type->isFloat()) as_.movaps(XMM0, static_cast<Xmm>(v->reg));
            else as_.movRR(RAX, static_cast<Reg>(v->reg));
        } else if (v && v->indirect) {
            as_.load(RAX, localMem(v), 8, false);
        } else {
            loadValue(e->type, genMem(e));
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
        if (c->intrinsic >= Intrinsic::AtomicAdd) {
            // Evaluate every argument into a temporary first (each may contain calls).
            std::vector<int32_t> slots;
            for (Expr* a : c->args) {
                genExpr(a);
                slots.push_back(allocTemp8());
                as_.store(Mem::at(RBP, slots.back()), RAX, 8);
            }
            Type* it = c->args[0]->type->elem;
            unsigned size = it->size;
            as_.load(R10, Mem::at(RBP, slots[0]), 8, false);   // pointer
            Mem target = Mem::at(R10, 0);
            switch (c->intrinsic) {
            case Intrinsic::AtomicAdd:
                as_.load(RCX, Mem::at(RBP, slots[1]), 8, false);
                as_.lockXadd(target, RCX, size);
                as_.movRR(RAX, RCX);
                normalize(it);
                break;
            case Intrinsic::AtomicCas:
                as_.load(RAX, Mem::at(RBP, slots[1]), 8, false);
                as_.load(RCX, Mem::at(RBP, slots[2]), 8, false);
                as_.lockCmpxchg(target, RCX, size);
                as_.setcc(CC_E, RAX);
                as_.movzx(RAX, RAX, 1);
                break;
            case Intrinsic::AtomicLoad:
                as_.load(RAX, target, size, it->isSigned);   // aligned loads are atomic on x86-64
                break;
            default:   // AtomicStore: an aligned store is atomic, and x86-64 keeps stores in order
                as_.load(RAX, Mem::at(RBP, slots[1]), 8, false);
                as_.store(target, RAX, size);
                break;
            }
        } else if (c->intrinsic == Intrinsic::DebugInfo) {
            as_.lea(RAX, Mem::ripRel(Section::Rdata, debugInfoOffset_));
        } else if (c->intrinsic == Intrinsic::FramePointer) {
            as_.movRR(RAX, RBP);
        } else if (c->intrinsic == Intrinsic::SqrtF32 || c->intrinsic == Intrinsic::SqrtF64) {
            bool f32 = c->intrinsic == Intrinsic::SqrtF32;
            int x = xmmOf(c->args[0]);
            if (x >= 0) {
                as_.sqrtS(XMM0, static_cast<Xmm>(x), f32);
            } else {
                genExpr(c->args[0]);
                as_.sqrtS(XMM0, XMM0, f32);
            }
        } else if (c->calleeValue) {
            Type* ft = c->calleeValue->type;
            genCall(ft->params, ft->ret, c->args, nullptr, c->calleeValue);
        } else {
            genCallTo(c->func, c->args);
        }
        break;
    }
    case ExprKind::Slice:
        genSlice(e->as<SliceExpr>());
        break;
    case ExprKind::Variant:
        genVariant(e->as<VariantExpr>());
        break;
    case ExprKind::Try:
        genTry(e->as<TryExpr>());
        break;
    case ExprKind::Pack:
        genPack(e->as<PackExpr>());
        break;
    case ExprKind::TypeOf:
        as_.lea(RAX, Mem::ripRel(Section::Rdata, typeInfo(e->as<TypeOfExpr>()->target)));
        break;
    case ExprKind::Index:
    case ExprKind::Member:
        loadValue(e->type, genMem(e));
        break;
    case ExprKind::Cast:
        genCast(e->as<CastExpr>());
        break;
    case ExprKind::StructLit:
        genStructLit(e->as<StructLitExpr>());
        break;
    case ExprKind::ArrayLit:
        genArrayLit(e->as<ArrayLitExpr>());
        break;
    case ExprKind::Path:
    case ExprKind::Sizeof:
        break;   // folded away by sema
    }
}

Cond CodeGen::genCompareFlags(BinaryExpr* e) {
    Type* t = e->lhs->type;
    if (t->isFloat()) {
        auto [a, b] = genOperandsFloat(e->lhs, e->rhs, false);
        as_.ucomis(a, b, t->size == 4);
        return condFor(e->op, false, true);
    }
    bool s = isSignedInt(t);
    if (e->rhs->kind == ExprKind::IntLit && fitsInt32(e->rhs->as<IntLitExpr>()->value)) {
        auto imm = static_cast<int32_t>(e->rhs->as<IntLitExpr>()->value);
        int l = regOf(e->lhs);
        Reg a = RAX;
        if (l >= 0) a = static_cast<Reg>(l);
        else genExpr(e->lhs);
        if (imm == 0 && (e->op == BinaryOp::Eq || e->op == BinaryOp::Ne)) as_.test(a, a);
        else as_.cmpImm(a, imm);
        return condFor(e->op, s, false);
    }
    auto [a, b] = genOperandsInt(e->lhs, e->rhs, false);
    as_.cmp(a, b);
    return condFor(e->op, s, false);
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
        auto [a, b] = genOperandsFloat(e->lhs, e->rhs);
        emitFloatOp(e->op, t->size == 4, a, b);
        return;
    }

    Type* lt = e->lhs->type;
    if (lt->isPointer()) {
        uint32_t s = lt->elem->size;
        if (e->rhs->type->isPointer()) {   // ptr - ptr = element count
            auto [a, b] = genOperandsInt(e->lhs, e->rhs);
            as_.sub(a, b);
            if (s != 1) {
                as_.movRI(RCX, s);
                as_.cqo();
                as_.idiv(RCX);
            }
            return;
        }
        if (e->rhs->kind == ExprKind::IntLit) {
            int64_t off = static_cast<int64_t>(e->rhs->as<IntLitExpr>()->value) * s;
            if (off >= INT32_MIN && off <= INT32_MAX) {
                genExpr(e->lhs);
                if (e->op == BinaryOp::Add) as_.addImm(RAX, static_cast<int32_t>(off));
                else as_.subImm(RAX, static_cast<int32_t>(off));
                return;
            }
        }
        auto [a, b] = genOperandsInt(e->lhs, e->rhs);
        if (s != 1) {
            as_.imulImm(RCX, b, static_cast<int32_t>(s));
            b = RCX;
        }
        if (e->op == BinaryOp::Add) as_.add(a, b);
        else as_.sub(a, b);
        return;
    }

    if (e->rhs->kind == ExprKind::IntLit) {
        genExpr(e->lhs);
        if (emitIntOpImm(e->op, t, RAX, e->rhs)) return;
        simpleInto(e->rhs, RCX);
        emitIntOp(e->op, t, RCX);
        return;
    }
    auto [a, b] = genOperandsInt(e->lhs, e->rhs);
    (void)a;
    emitIntOp(e->op, t, b);
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
        loadValue(t, genMem(e));
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
    int32_t step = t->isPointer() ? static_cast<int32_t>(t->elem->size) : 1;
    bool f32 = t->size == 4;
    auto one = [&] { return Mem::ripRel(Section::Rdata, f32 ? addF32(1.0f) : addF64(1.0)); };

    // Register variable
    LocalVar* v = e->operand->kind == ExprKind::Ident ? e->operand->as<IdentExpr>()->local : nullptr;
    if (v && v->reg >= 0) {
        if (t->isFloat()) {
            auto x = static_cast<Xmm>(v->reg);
            if (post) as_.movaps(XMM0, x);
            as_.movsLoad(XMM1, one(), f32);
            if (inc) as_.addS(x, XMM1, f32);
            else as_.subS(x, XMM1, f32);
            if (!post) as_.movaps(XMM0, x);
            return;
        }
        auto r = static_cast<Reg>(v->reg);
        if (post) as_.movRR(RAX, r);
        if (inc) as_.addImm(r, step);
        else as_.subImm(r, step);
        normalize(t, r);
        if (!post) as_.movRR(RAX, r);
        return;
    }

    Mem m;
    if (!stableMem(e->operand, m)) {
        genAddr(e->operand);
        as_.movRR(R10, RAX);
        m = Mem::at(R10, 0);
    }
    if (t->isFloat()) {
        as_.movsLoad(XMM0, m, f32);
        as_.movsLoad(XMM1, one(), f32);
        if (post) as_.movaps(XMM2, XMM0);
        if (inc) as_.addS(XMM0, XMM1, f32);
        else as_.subS(XMM0, XMM1, f32);
        as_.movsStore(m, XMM0, f32);
        if (post) as_.movaps(XMM0, XMM2);
        return;
    }
    as_.load(RAX, m, t->size, isSignedInt(t));
    as_.movRR(RDX, RAX);
    if (inc) as_.addImm(RAX, step);
    else as_.subImm(RAX, step);
    normalize(t);
    as_.store(m, RAX, t->size);
    if (post) as_.movRR(RAX, RDX);
}

void CodeGen::genAssign(AssignExpr* e) {
    Type* t = e->target->type;
    bool f32 = t->size == 4;

    // Target is a register variable.
    LocalVar* v = e->target->kind == ExprKind::Ident ? e->target->as<IdentExpr>()->local : nullptr;
    if (v && v->reg >= 0) {
        if (t->isFloat()) {
            auto x = static_cast<Xmm>(v->reg);
            if (!e->compound) {
                genExpr(e->value);
                as_.movaps(x, XMM0);
                return;
            }
            Xmm src;
            int vx = xmmOf(e->value);
            if (vx >= 0) {
                src = static_cast<Xmm>(vx);
            } else if (isSimpleFloat(e->value)) {
                simpleFloatInto(e->value, XMM1);
                src = XMM1;
            } else {
                genExpr(e->value);
                src = XMM0;
            }
            emitFloatOp(e->op, f32, x, src);
            as_.movaps(XMM0, x);
            return;
        }
        auto r = static_cast<Reg>(v->reg);
        if (!e->compound) {
            genExpr(e->value);
            as_.movRR(r, RAX);
            return;
        }
        if (t->isPointer()) {
            uint32_t s = t->elem->size;
            genExpr(e->value);
            if (s != 1) as_.imulImm(RAX, RAX, static_cast<int32_t>(s));
            if (e->op == BinaryOp::Add) as_.add(r, RAX);
            else as_.sub(r, RAX);
            as_.movRR(RAX, r);
            return;
        }
        if (emitIntOpImm(e->op, t, r, e->value)) {
            as_.movRR(RAX, r);
            return;
        }
        int vr = regOf(e->value);
        bool simpleOp = e->op == BinaryOp::Add || e->op == BinaryOp::Sub || e->op == BinaryOp::BitAnd ||
                        e->op == BinaryOp::BitOr || e->op == BinaryOp::BitXor || e->op == BinaryOp::Mul;
        if (simpleOp) {
            Reg src;
            if (vr >= 0) {
                src = static_cast<Reg>(vr);
            } else {
                genExpr(e->value);
                src = RAX;
            }
            switch (e->op) {
            case BinaryOp::Add: as_.add(r, src); break;
            case BinaryOp::Sub: as_.sub(r, src); break;
            case BinaryOp::BitAnd: as_.and_(r, src); break;
            case BinaryOp::BitOr: as_.or_(r, src); break;
            case BinaryOp::BitXor: as_.xor_(r, src); break;
            default: as_.imul(r, src); break;
            }
            normalize(t, r);
            as_.movRR(RAX, r);
            return;
        }
        genExpr(e->value);
        as_.movRR(RCX, RAX);
        as_.movRR(RAX, r);
        emitIntOp(e->op, t, RCX);
        as_.movRR(r, RAX);
        return;
    }

    // Stable memory target ([rbp+x], [reg+idx*s]): evaluate the value, then store directly.
    Mem m;
    bool stable = stableMem(e->target, m);
    if (!stable) {
        genAddr(e->target);
        int32_t addrSlot = allocTemp8();
        as_.store(Mem::at(RBP, addrSlot), RAX, 8);
        genExpr(e->value);
        as_.load(R10, Mem::at(RBP, addrSlot), 8, false);
        m = Mem::at(R10, 0);
    } else {
        genExpr(e->value);
    }

    if (!e->compound) {
        storeValue(t, m);
        if (t->isAggregate()) as_.lea(RAX, m);
        return;
    }
    if (t->isFloat()) {
        as_.movaps(XMM1, XMM0);
        as_.movsLoad(XMM0, m, f32);
        emitFloatOp(e->op, f32, XMM0, XMM1);
        as_.movsStore(m, XMM0, f32);
        return;
    }
    as_.movRR(RCX, RAX);
    as_.load(RAX, m, t->size, isSignedInt(t));
    if (t->isPointer()) {
        if (t->elem->size != 1) as_.imulImm(RCX, RCX, static_cast<int32_t>(t->elem->size));
        if (e->op == BinaryOp::Add) as_.add(RAX, RCX);
        else as_.sub(RAX, RCX);
    } else {
        emitIntOp(e->op, t, RCX);   // only clobbers RDX besides RAX/RCX
    }
    as_.store(m, RAX, t->size);
}

void CodeGen::genCallTo(FuncDecl* f, const std::vector<Expr*>& args) {
    genCall(f->paramTypes, f->returnType, args, f, nullptr);
}

void CodeGen::genFunctionAddress(FuncDecl* f) {
    if (!f->externDll.empty()) {
        // Imported function: its address is the IAT entry filled in by the loader.
        as_.load(RAX, Mem::ripRel(Section::Import, importSlot(f->externDll, f->name)), 8, false);
        return;
    }
    as_.lea(RAX, Mem::ripRel(Section::Text, 0));
    funcRefs_.push_back({as_.fixups.size() - 1, f});
}

void CodeGen::genCall(const std::vector<Type*>& paramTypes, Type* rt, const std::vector<Expr*>& args, FuncDecl* direct,
                      Expr* calleeValue) {
    bool hidden = passedByRef(rt);
    size_t n = args.size() + (hidden ? 1 : 0);
    maxOutgoing_ = std::max<int32_t>(maxOutgoing_, static_cast<int32_t>(8 * std::max<size_t>(4, n)));

    int32_t retSlot = hidden ? allocSlot(rt->size, std::max<uint32_t>(rt->align, 8)) : 0;
    int32_t calleeSlot = 0;
    if (calleeValue) {
        // The function value is evaluated first, before the arguments.
        genExpr(calleeValue);
        calleeSlot = allocTemp8();
        as_.store(Mem::at(RBP, calleeSlot), RAX, 8);
    }

    // Arguments that can be re-read for free (literals, variables) are loaded
    // straight into their argument register later; others are evaluated into
    // temporaries first, since later arguments may contain calls.
    std::vector<int32_t> slots(args.size(), 0);
    std::vector<bool> deferred(args.size(), false);
    for (size_t i = 0; i < args.size(); ++i) {
        Type* pt = paramTypes[i];
        if (pt->isFloat() ? isSimpleFloat(args[i]) : (pt->isScalar() && isSimple(args[i]))) {
            deferred[i] = true;
            continue;
        }
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
                copyMem(Mem::at(RBP, copy), Mem::at(RAX, 0), pt->size);
                as_.lea(RAX, Mem::at(RBP, copy));
            }
            as_.store(slot, RAX, 8);
        } else {
            as_.store(slot, RAX, 8);
        }
    }

    // Stack arguments first: they go through RAX/XMM0, which hold register arguments later.
    std::vector<size_t> order;
    for (size_t k = 4; k < n; ++k) order.push_back(k);
    for (size_t k = 0; k < n && k < 4; ++k) order.push_back(k);
    for (size_t k : order) {
        if (hidden && k == 0) {
            as_.lea(RCX, Mem::at(RBP, retSlot));
            continue;
        }
        size_t i = k - (hidden ? 1 : 0);
        Type* pt = paramTypes[i];
        bool f32 = pt->size == 4;
        if (deferred[i]) {
            if (k < 4) {
                if (pt->isFloat()) simpleFloatInto(args[i], static_cast<Xmm>(k));
                else simpleInto(args[i], kArgRegs[k]);
            } else if (pt->isFloat()) {
                simpleFloatInto(args[i], XMM0);
                as_.movsStore(Mem::at(RSP, static_cast<int32_t>(8 * k)), XMM0, f32);
            } else {
                simpleInto(args[i], RAX);
                as_.store(Mem::at(RSP, static_cast<int32_t>(8 * k)), RAX, 8);
            }
            continue;
        }
        Mem slot = Mem::at(RBP, slots[i]);
        if (k < 4) {
            if (pt->isFloat()) as_.movsLoad(static_cast<Xmm>(k), slot, f32);
            else as_.load(kArgRegs[k], slot, 8, false);
        } else {
            as_.load(RAX, slot, 8, false);
            as_.store(Mem::at(RSP, static_cast<int32_t>(8 * k)), RAX, 8);
        }
    }

    if (calleeValue) {
        as_.load(RAX, Mem::at(RBP, calleeSlot), 8, false);
        as_.callR(RAX);
    } else if (!direct->externDll.empty()) {
        as_.callMem(Mem::ripRel(Section::Import, importSlot(direct->externDll, direct->name)));
    } else {
        as_.call(funcLabels_[direct]);
    }

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
    if (to->isFloat() && from->isFloat()) {
        int x = xmmOf(e->operand);
        if (x >= 0) as_.movaps(XMM0, static_cast<Xmm>(x));
        else genExpr(e->operand);
        if (from->size == 4 && to->size == 8) as_.cvtss2sd(XMM0, XMM0);
        else if (from->size == 8 && to->size == 4) as_.cvtsd2ss(XMM0, XMM0);
        return;
    }
    if (to->isSlice() && from->isArray()) {
        genExpr(e->operand);   // address of the array
        int32_t slot = allocSlot(16, 8);
        as_.store(Mem::at(RBP, slot), RAX, 8);
        as_.movRI(RCX, from->count);
        as_.store(Mem::at(RBP, slot + 8), RCX, 8);
        as_.lea(RAX, Mem::at(RBP, slot));
        return;
    }
    if (to->isFloat()) {
        int r = regOf(e->operand);
        if (r >= 0) {
            as_.cvtsi2s(XMM0, static_cast<Reg>(r), to->size == 4);
        } else {
            genExpr(e->operand);
            as_.cvtsi2s(XMM0, RAX, to->size == 4);
        }
        return;
    }
    genExpr(e->operand);
    if (from == to) return;
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

// base[lo..hi] → { base.ptr + lo, hi - lo } built in a 16-byte temporary
void CodeGen::genSlice(SliceExpr* e) {
    Type* bt = e->base->type;
    uint32_t elemSize = bt->elem->size;
    int32_t slot = allocSlot(16, 8);
    Mem ptrField = Mem::at(RBP, slot);
    Mem lenField = Mem::at(RBP, slot + 8);
    if (bt->isSlice()) {
        Mem m = genMem(e->base);
        Mem lenMem = m;
        addDisp(lenMem, 8);
        as_.load(RCX, lenMem, 8, false);
        as_.load(RAX, m, 8, false);
        as_.store(lenField, RCX, 8);
    } else {
        if (bt->isArray()) genAddr(e->base);
        else genExpr(e->base);
        if (bt->isArray()) {
            as_.movRI(RCX, bt->count);
            as_.store(lenField, RCX, 8);
        }
    }
    as_.store(ptrField, RAX, 8);
    bool check = boundsChecks && !bt->isPointer();
    if (e->hi) {
        genExpr(e->hi);
        if (check) boundsCheck(RAX, false, 0, slot + 8, true, e->loc);   // hi <= len
        as_.store(lenField, RAX, 8);
    }
    if (e->lo) {
        genExpr(e->lo);
        if (boundsChecks) boundsCheck(RAX, false, 0, slot + 8, true, e->loc);   // lo <= hi
        as_.load(R10, ptrField, 8, false);
        as_.imulImm(RCX, RAX, static_cast<int32_t>(elemSize));
        as_.add(R10, RCX);
        as_.store(ptrField, R10, 8);
        as_.load(R11, lenField, 8, false);
        as_.sub(R11, RAX);
        as_.store(lenField, R11, 8);
    }
    as_.lea(RAX, ptrField);
}

// ───────────── Runtime error reporting ─────────────

namespace {
void put32At(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
void put64At(std::vector<uint8_t>& b, size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
} // namespace

std::string CodeGen::shortFileName(uint32_t fileId) const {
    if (!sources || fileId >= sources->count()) return "?";
    const std::string& p = sources->get(fileId).path;
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// if (value < limit) (or <= with allowEqual), unsigned: continue; else __ron_bounds_fail(value, limit, file, line)
void CodeGen::boundsCheck(Reg value, bool constLimit, uint64_t limit, int32_t limitSlot, bool allowEqual, SourceLoc loc) {
    if (!sema_.boundsFail) return;
    Label ok = as_.newLabel();
    if (constLimit && fitsInt32(limit)) {
        as_.cmpImm(value, static_cast<int32_t>(limit));
    } else {
        if (constLimit) as_.movRI(R11, limit);
        else as_.load(R11, Mem::at(RBP, limitSlot), 8, false);
        as_.cmp(value, R11);
    }
    as_.jcc(allowEqual ? CC_BE : CC_B, ok);
    // Failure path (never returns)
    as_.movRR(RCX, value);
    if (constLimit) as_.movRI(RDX, limit);
    else as_.load(RDX, Mem::at(RBP, limitSlot), 8, false);
    as_.lea(R8, Mem::ripRel(Section::Rdata, addString(shortFileName(loc.fileId))));
    as_.movRI(R9, loc.line);
    as_.call(funcLabels_[sema_.boundsFail]);
    as_.bind(ok);
}

// Function and line tables read by std/runtime.ron (layout must match it):
//   DebugInfo { i64 func_count; FuncEntry* funcs; i64 line_count; LineEntry* lines; }
//   FuncEntry { u64 start; u64 end; u8* name; u8* file; }   LineEntry { u64 address; u8* file; i64 line; }
void CodeGen::emitDebugInfo() {
    uint32_t lines = rdataAlloc(static_cast<uint32_t>(std::max<size_t>(1, lineMarks_.size()) * 24), 8);
    for (size_t i = 0; i < lineMarks_.size(); ++i) {
        uint32_t at = lines + static_cast<uint32_t>(i * 24);
        image_.absFixups.push_back({Section::Rdata, at, Section::Text, lineMarks_[i].pos});
        rdataPointer(at + 8, addString(shortFileName(lineMarks_[i].fileId)));
        put64At(image_.rdata, at + 16, lineMarks_[i].line);
    }
    uint32_t funcs = rdataAlloc(static_cast<uint32_t>(std::max<size_t>(1, funcRanges_.size()) * 32), 8);
    for (size_t i = 0; i < funcRanges_.size(); ++i) {
        uint32_t at = funcs + static_cast<uint32_t>(i * 32);
        image_.absFixups.push_back({Section::Rdata, at, Section::Text, funcRanges_[i].start});
        image_.absFixups.push_back({Section::Rdata, at + 8, Section::Text, funcRanges_[i].end});
        rdataPointer(at + 16, addString(funcRanges_[i].name));
        rdataPointer(at + 24, addString(shortFileName(funcRanges_[i].fileId)));
    }
    put64At(image_.rdata, debugInfoOffset_, funcRanges_.size());
    rdataPointer(debugInfoOffset_ + 8, funcs);
    put64At(image_.rdata, debugInfoOffset_ + 16, lineMarks_.size());
    rdataPointer(debugInfoOffset_ + 24, lines);
}

// ───────────── Runtime type information ─────────────


uint32_t CodeGen::rdataAlloc(uint32_t size, uint32_t align) {
    while (image_.rdata.size() % align) image_.rdata.push_back(0);
    auto off = static_cast<uint32_t>(image_.rdata.size());
    image_.rdata.resize(image_.rdata.size() + size, 0);
    return off;
}

void CodeGen::rdataPointer(uint32_t at, uint32_t targetOffset) {
    image_.absFixups.push_back({Section::Rdata, at, Section::Rdata, targetOffset});
}

// Emits (once per type) a TypeInfo record matching the prelude's layout; returns its .rdata offset.
uint32_t CodeGen::typeInfo(Type* t) {
    auto memo = typeInfos_.find(t);
    if (memo != typeInfos_.end()) return memo->second;
    Type* infoType = sema_.typeInfoType();
    StructDecl* info = infoType->structDecl;
    auto field = [&](const char* name) -> uint32_t {
        for (const FieldDecl& f : info->fields)
            if (f.name == name) return f.offset;
        return 0;
    };
    uint32_t rec = rdataAlloc(infoType->size, 8);
    typeInfos_[t] = rec;   // registered first: recursive types point back to it

    enum Kind { Void, Bool, Int, Float, Pointer, Array, Struct, Enum, Sum, Function, Slice };
    int kind = Void;
    switch (t->kind) {
    case TypeKind::Void: kind = Void; break;
    case TypeKind::Bool: kind = Bool; break;
    case TypeKind::Int: kind = t->enumDecl ? Enum : Int; break;
    case TypeKind::Float: kind = Float; break;
    case TypeKind::Pointer:
    case TypeKind::Null: kind = Pointer; break;
    case TypeKind::Array: kind = Array; break;
    case TypeKind::Struct: kind = t->sumDecl ? Sum : Struct; break;
    case TypeKind::Function: kind = Function; break;
    case TypeKind::Slice: kind = Slice; break;
    }
    put32At(image_.rdata, rec + field("kind"), static_cast<uint32_t>(kind));
    put32At(image_.rdata, rec + field("size"), t->size);
    image_.rdata[rec + field("is_signed")] = t->isInt() && t->isSigned ? 1 : 0;
    rdataPointer(rec + field("name"), addString(typeName(t)));

    std::vector<std::string> names;
    std::vector<int64_t> values;
    std::vector<Type*> types;
    if (t->enumDecl) {
        for (const EnumMember& m : t->enumDecl->members) {
            names.push_back(m.name);
            values.push_back(m.resolved);
        }
    } else if (t->sumDecl) {
        for (const EnumMember& m : t->sumDecl->members) names.push_back(m.name);
    } else if (t->isStruct()) {
        for (const FieldDecl& f : t->structDecl->fields) {
            if (!f.type) continue;
            names.push_back(f.name);
            values.push_back(f.offset);
            types.push_back(f.type);
        }
    }
    uint32_t count = t->isArray() ? static_cast<uint32_t>(t->count) : static_cast<uint32_t>(names.size());
    put32At(image_.rdata, rec + field("count"), count);
    if (!names.empty()) {
        uint32_t arr = rdataAlloc(static_cast<uint32_t>(names.size() * 8), 8);
        for (size_t i = 0; i < names.size(); ++i) rdataPointer(arr + static_cast<uint32_t>(i * 8), addString(names[i]));
        rdataPointer(rec + field("names"), arr);
    }
    if (!values.empty()) {
        uint32_t arr = rdataAlloc(static_cast<uint32_t>(values.size() * 8), 8);
        for (size_t i = 0; i < values.size(); ++i) put64At(image_.rdata, arr + i * 8, static_cast<uint64_t>(values[i]));
        rdataPointer(rec + field("values"), arr);
    }
    if (!types.empty()) {
        uint32_t arr = rdataAlloc(static_cast<uint32_t>(types.size() * 8), 8);
        for (size_t i = 0; i < types.size(); ++i) rdataPointer(arr + static_cast<uint32_t>(i * 8), typeInfo(types[i]));
        rdataPointer(rec + field("types"), arr);
    }
    if (t->elem) rdataPointer(rec + field("elem"), typeInfo(t->elem));
    return rec;
}

// Extra arguments of a variadic call → temporary array → slice { ptr, len }.
void CodeGen::genPack(PackExpr* p) {
    int32_t slice = allocSlot(16, 8);
    size_t n = p->elems.size();
    if (n == 0) {
        zeroMem(Mem::at(RBP, slice), 16);
        as_.lea(RAX, Mem::at(RBP, slice));
        return;
    }
    uint32_t elemSize = p->elemType->size;
    int32_t arr = allocSlot(static_cast<uint32_t>(n * elemSize), std::max<uint32_t>(p->elemType->align, 8));
    for (size_t i = 0; i < n; ++i) {
        Expr* x = p->elems[i];
        int32_t at = arr + static_cast<int32_t>(i * elemSize);
        genExpr(x);
        if (p->boxAny && x->type != p->elemType) {
            // Box: copy the value into its own slot, then store Any{&slot, &type_info}.
            int32_t valueSlot = allocSlot(std::max<uint32_t>(x->type->size, 1), std::max<uint32_t>(x->type->align, 8));
            storeValue(x->type, Mem::at(RBP, valueSlot));
            as_.lea(RAX, Mem::at(RBP, valueSlot));
            as_.store(Mem::at(RBP, at), RAX, 8);
            as_.lea(RAX, Mem::ripRel(Section::Rdata, typeInfo(x->type)));
            as_.store(Mem::at(RBP, at + 8), RAX, 8);
        } else {
            storeValue(p->elemType, Mem::at(RBP, at));
        }
    }
    as_.lea(RAX, Mem::at(RBP, arr));
    as_.store(Mem::at(RBP, slice), RAX, 8);
    as_.movRI(RCX, n);
    as_.store(Mem::at(RBP, slice + 8), RCX, 8);
    as_.lea(RAX, Mem::at(RBP, slice));
}

void CodeGen::genArrayLit(ArrayLitExpr* e) {
    Type* t = e->type;
    Type* elem = t->elem;
    int32_t slot = allocSlot(t->size, std::max<uint32_t>(t->align, 8));
    if (e->elems.size() < t->count) zeroMem(Mem::at(RBP, slot), t->size);
    for (size_t i = 0; i < e->elems.size(); ++i) {
        genExpr(e->elems[i]);
        storeValue(elem, Mem::at(RBP, slot + static_cast<int32_t>(i * elem->size)));
    }
    as_.lea(RAX, Mem::at(RBP, slot));
}

void CodeGen::genStructLit(StructLitExpr* e) {
    Type* t = e->type;
    int32_t slot = allocSlot(t->size, std::max<uint32_t>(t->align, 8));
    StructDecl* s = t->structDecl;
    // Zero only when padding or omitted fields could stay uninitialized.
    uint32_t covered = 0;
    for (const FieldDecl& f : s->fields) covered += f.type ? f.type->size : 0;
    bool omitted = false;
    for (Expr* f : e->fields) omitted |= f == nullptr;
    if (e->fields.empty() || omitted || covered != t->size) zeroMem(Mem::at(RBP, slot), t->size);
    for (size_t i = 0; i < e->fields.size(); ++i) {
        if (!e->fields[i]) continue;   // named initializer left this field out: zero
        genExpr(e->fields[i]);
        storeValue(s->fields[i].type, Mem::at(RBP, slot + static_cast<int32_t>(s->fields[i].offset)));
    }
    as_.lea(RAX, Mem::at(RBP, slot));
}

} // namespace ron
