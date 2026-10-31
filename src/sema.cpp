#include "sema.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ron {

namespace {

// Mathematical value of an untyped integer literal, if it fits in int64.
bool literalAsInt64(const IntLitExpr* e, int64_t& out) {
    if (e->negative) {
        out = static_cast<int64_t>(e->value);
        return true;
    }
    if (e->value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
    out = static_cast<int64_t>(e->value);
    return true;
}

bool literalFits(const IntLitExpr* e, const Type* t) {
    if (!t->isInt()) return false;
    unsigned bits = t->size * 8;
    if (t->isSigned) {
        int64_t maxV = bits == 64 ? std::numeric_limits<int64_t>::max() : (int64_t(1) << (bits - 1)) - 1;
        int64_t minV = bits == 64 ? std::numeric_limits<int64_t>::min() : -(int64_t(1) << (bits - 1));
        if (e->negative) return static_cast<int64_t>(e->value) >= minV;
        return e->value <= static_cast<uint64_t>(maxV);
    }
    if (e->negative) return false;
    uint64_t maxU = bits == 64 ? std::numeric_limits<uint64_t>::max() : (uint64_t(1) << bits) - 1;
    return e->value <= maxU;
}

// Wraps a 64-bit pattern to the width of `t` (sign- or zero-extended back to 64 bits).
uint64_t normalizeInt(uint64_t v, const Type* t) {
    unsigned bits = t->size * 8;
    if (bits >= 64) return v;
    uint64_t mask = (uint64_t(1) << bits) - 1;
    v &= mask;
    if (t->isSigned && (v >> (bits - 1)) & 1) v |= ~mask;
    return v;
}

// A conditional whose branches are both untyped literals is itself untyped:
// `c ? 1.0 : -1.0` takes its type from the context, like a plain literal.
bool isUntypedTernary(const Expr* e);
bool isUntypedInt(const Expr* e) {
    if (e->kind == ExprKind::Ternary)
        return isUntypedTernary(e) && isUntypedInt(e->as<TernaryExpr>()->thenExpr) &&
               isUntypedInt(e->as<TernaryExpr>()->elseExpr);
    return e->kind == ExprKind::IntLit && e->as<IntLitExpr>()->untyped;
}
bool isUntypedFloat(const Expr* e) {
    if (e->kind == ExprKind::Ternary) return isUntypedTernary(e) && !isUntypedInt(e);
    return e->kind == ExprKind::FloatLit && e->as<FloatLitExpr>()->untyped;
}
bool isUntyped(const Expr* e) { return isUntypedInt(e) || isUntypedFloat(e); }
bool isUntypedTernary(const Expr* e) {
    auto* t = e->as<TernaryExpr>();
    return t->thenExpr && t->elseExpr && t->thenExpr->type && t->elseExpr->type && isUntyped(t->thenExpr) &&
           isUntyped(t->elseExpr);
}

std::string operatorFuncName(BinaryOp op) { return std::string("op") + binaryOpText(op); }

std::string displayName(const std::string& fn) {
    return fn.rfind("op", 0) == 0 && fn.size() > 2 && !std::isalpha(static_cast<unsigned char>(fn[2]))
               ? "operator " + fn.substr(2)
               : fn;
}

} // namespace

Sema::Sema(Program& program, AstArena& arena, TypeContext& types, Diagnostics& diags)
    : program_(program), arena_(arena), types_(types), diags_(diags) {}

// ───────────────────────────── Driver ─────────────────────────────

bool Sema::run() {
    collect();
    for (auto& [name, s] : structs_)
        if (s->typeParams.empty()) layoutStruct(s);
    for (auto& [name, e] : enums_) resolveEnum(e);
    for (Decl* d : program_.decls)
        if (d->declKind == Decl::Kind::Global) resolveGlobal(static_cast<GlobalVar*>(d));
    for (Decl* d : program_.decls)
        if (d->declKind == Decl::Kind::Func && static_cast<FuncDecl*>(d)->typeParams.empty())
            resolveSignature(static_cast<FuncDecl*>(d));

    // Reject duplicate overloads (same parameter types).
    for (auto& [name, list] : funcs_) {
        for (size_t i = 0; i < list.size(); ++i)
            for (size_t j = i + 1; j < list.size(); ++j)
                if (list[i]->typeParams.empty() && list[j]->typeParams.empty() &&
                    list[i]->paramTypes == list[j]->paramTypes) {
                    diags_.error(list[j]->loc, "function '" + displayName(name) +
                                                   "' is already defined with the same parameter types");
                    diags_.note(list[i]->loc, "previous definition is here");
                }
    }

    std::vector<FuncDecl*> bodies;
    for (Decl* d : program_.decls) {
        if (d->declKind != Decl::Kind::Func) continue;
        auto* f = static_cast<FuncDecl*>(d);
        if (f->body && f->typeParams.empty()) {
            checkFunction(f);
            bodies.push_back(f);
        }
    }

    // Generic instances discovered while checking (instances may create more).
    while (!pendingInstances_.empty()) {
        FuncDecl* f = pendingInstances_.front();
        pendingInstances_.erase(pendingInstances_.begin());
        size_t errorsBefore = diags_.errorCount();
        pushTypeEnv(f->genericOrigin->typeParams, f->typeArgs);
        checkFunction(f);
        popTypeEnv();
        if (diags_.errorCount() > errorsBefore)
            diags_.note(instantiatedAt_[f], "in the instantiation of '" + instanceName(f->name, f->typeArgs) + "'");
        bodies.push_back(f);
    }

    auto it = funcs_.find("main");
    if (it == funcs_.end() || it->second.empty()) {
        diags_.error(SourceLoc{0, 1, 1, 0}, "no 'main' function found (expected: fn main() -> int { ... })");
    } else {
        mainFunc = it->second[0];
        if (!mainFunc->params.empty() || !mainFunc->body)
            diags_.error(mainFunc->loc, "'main' must take no parameters and have a body");
        else if (mainFunc->returnType && !mainFunc->returnType->isVoid() && !mainFunc->returnType->isInt())
            diags_.error(mainFunc->loc, "'main' must return 'int' or nothing");
    }

    // Dead-code elimination: only functions reachable from main are compiled,
    // so importing a library costs nothing for the parts you do not call.
    if (mainFunc) {
        std::vector<FuncDecl*> work{mainFunc};
        mainFunc->used = true;
        while (!work.empty()) {
            FuncDecl* f = work.back();
            work.pop_back();
            for (FuncDecl* c : f->callees)
                if (!c->used) {
                    c->used = true;
                    work.push_back(c);
                }
        }
    }
    for (FuncDecl* f : bodies)
        if (f->used) functions.push_back(f);
    for (auto& [name, list] : funcs_)
        for (FuncDecl* f : list)
            if (!f->body && f->used) externs.push_back(f);

    return !diags_.hasErrors();
}

void Sema::collect() {
    auto typeNameTaken = [&](const std::string& name) {
        return types_.builtin(name) || structs_.count(name) || enums_.count(name);
    };
    for (Decl* d : program_.decls) {
        switch (d->declKind) {
        case Decl::Kind::Struct: {
            auto* s = static_cast<StructDecl*>(d);
            if (typeNameTaken(s->name)) {
                diags_.error(s->loc, "type '" + s->name + "' is already defined");
                break;
            }
            if (s->typeParams.empty()) s->type = types_.newStruct(s->name, s);   // templates get types per instance
            structs_[s->name] = s;
            break;
        }
        case Decl::Kind::Enum: {
            auto* e = static_cast<EnumDecl*>(d);
            if (typeNameTaken(e->name)) {
                diags_.error(e->loc, "type '" + e->name + "' is already defined");
                break;
            }
            e->type = types_.newEnum(e->name, e);
            enums_[e->name] = e;
            break;
        }
        case Decl::Kind::Func: {
            auto* f = static_cast<FuncDecl*>(d);
            funcs_[f->name].push_back(f);
            break;
        }
        case Decl::Kind::Global: {
            auto* g = static_cast<GlobalVar*>(d);
            if (globalsByName_.count(g->name)) {
                diags_.error(g->loc, "global '" + g->name + "' is already defined");
                break;
            }
            globalsByName_[g->name] = g;
            break;
        }
        case Decl::Kind::Import:
            break;   // handled by the driver before sema
        }
    }
}

// ───────────────────────────── Types ─────────────────────────────

Type* Sema::resolveType(TypeExpr* t) {
    switch (t->kind) {
    case TypeExpr::Kind::Named: {
        if (t->typeArgs.empty() && !typeEnv_.empty()) {
            auto& env = typeEnv_.back();
            if (auto it = env.find(t->name); it != env.end()) return it->second;
        }
        if (Type* b = types_.builtin(t->name)) {
            if (!t->typeArgs.empty()) diags_.error(t->loc, "type '" + t->name + "' does not take type arguments");
            return b;
        }
        if (auto it = structs_.find(t->name); it != structs_.end()) {
            StructDecl* s = it->second;
            if (s->typeParams.empty()) {
                if (!t->typeArgs.empty()) {
                    diags_.error(t->loc, "struct '" + s->name + "' is not generic");
                    return nullptr;
                }
                return s->type;
            }
            if (t->typeArgs.size() != s->typeParams.size()) {
                diags_.error(t->loc, "generic struct '" + s->name + "' needs " + std::to_string(s->typeParams.size()) +
                                         " type argument(s), e.g. " + s->name + "<int>");
                return nullptr;
            }
            std::vector<Type*> args;
            for (TypeExpr* a : t->typeArgs) {
                Type* r = resolveType(a);
                if (!r) return nullptr;
                args.push_back(r);
            }
            return instantiateStruct(s, args);
        }
        if (auto it = enums_.find(t->name); it != enums_.end()) return it->second->type;
        diags_.error(t->loc, "unknown type '" + t->name + "'");
        return nullptr;
    }
    case TypeExpr::Kind::Pointer: {
        Type* inner = resolveType(t->inner);
        return inner ? types_.pointerTo(inner) : nullptr;
    }
    case TypeExpr::Kind::Array: {
        Type* inner = resolveType(t->inner);
        if (!inner) return nullptr;
        if (inner->isStruct() && !inner->complete) layoutStruct(inner->structDecl);
        if (!inner->complete) return nullptr;
        if (inner->isVoid()) {
            diags_.error(t->loc, "cannot make an array of void");
            return nullptr;
        }
        if (!t->sizeConst.empty()) {
            auto it = globalsByName_.find(t->sizeConst);
            if (it == globalsByName_.end() || !it->second->isConst) {
                diags_.error(t->loc, "array size '" + t->sizeConst + "' must be an integer const");
                return nullptr;
            }
            resolveGlobal(it->second);
            GlobalVar* g = it->second;
            if (!g->type || !g->type->isInt() || !g->hasValue ||
                (g->type->isSigned && static_cast<int64_t>(g->intValue) <= 0) || g->intValue == 0) {
                diags_.error(t->loc, "array size '" + t->sizeConst + "' must be a positive integer const");
                return nullptr;
            }
            t->arraySize = g->intValue;
            t->sizeConst.clear();
        }
        if (inner->size * t->arraySize > 0x7FFFFFFFull) {
            diags_.error(t->loc, "array is too large");
            return nullptr;
        }
        return types_.arrayOf(inner, t->arraySize);
    }
    }
    return nullptr;
}

void Sema::layoutStruct(StructDecl* s) {
    int& state = structState_[s];
    if (state == 2) return;
    if (state == 1) {
        diags_.error(s->loc, "struct '" + s->name + "' contains itself (use a pointer: " + s->name + "*)");
        return;
    }
    state = 1;
    // Fields of a generic instance are resolved with its type arguments.
    bool generic = s->genericOrigin != nullptr;
    if (generic) pushTypeEnv(s->genericOrigin->typeParams, s->typeArgs);
    uint32_t offset = 0, align = 1;
    std::map<std::string, bool> seen;
    for (FieldDecl& f : s->fields) {
        if (seen[f.name]) diags_.error(f.loc, "duplicate field '" + f.name + "'");
        seen[f.name] = true;
        Type* t = resolveType(f.typeExpr);
        if (!t) continue;
        if (t->isStruct() && !t->complete) {
            layoutStruct(t->structDecl);
            if (!t->complete) continue;
        }
        if (t->isVoid()) {
            diags_.error(f.loc, "a field cannot have type void");
            continue;
        }
        f.type = t;
        offset = (offset + t->align - 1) / t->align * t->align;
        f.offset = offset;
        offset += t->size;
        align = std::max(align, t->align);
    }
    if (generic) popTypeEnv();
    s->type->size = std::max<uint32_t>(1, (offset + align - 1) / align * align);
    s->type->align = align;
    s->type->complete = true;
    structState_[s] = 2;
}

// ───────────────────────────── Generics ─────────────────────────────

void Sema::pushTypeEnv(const std::vector<std::string>& params, const std::vector<Type*>& args) {
    std::map<std::string, Type*> env;
    for (size_t i = 0; i < params.size() && i < args.size(); ++i) env[params[i]] = args[i];
    typeEnv_.push_back(std::move(env));
}

std::string Sema::instanceName(const std::string& base, const std::vector<Type*>& args) {
    std::string n = base + "<";
    for (size_t i = 0; i < args.size(); ++i) n += (i ? ", " : "") + typeName(args[i]);
    return n + ">";
}

Type* Sema::instantiateStruct(StructDecl* g, const std::vector<Type*>& args) {
    auto key = std::make_pair(g, args);
    if (auto it = structInstances_.find(key); it != structInstances_.end()) return it->second->type;
    auto* inst = arena_.make<StructDecl>(g->loc);
    inst->name = instanceName(g->name, args);
    inst->genericOrigin = g;
    inst->typeArgs = args;
    for (const FieldDecl& f : g->fields) {
        FieldDecl copy;
        copy.typeExpr = f.typeExpr;
        copy.name = f.name;
        copy.loc = f.loc;
        inst->fields.push_back(copy);
    }
    inst->type = types_.newStruct(inst->name, inst);
    structInstances_[key] = inst;   // registered before layout: List<T>* next is allowed
    layoutStruct(inst);
    return inst->type;
}

FuncDecl* Sema::instantiateFunction(FuncDecl* g, const std::vector<Type*>& args, SourceLoc at) {
    auto key = std::make_pair(g, args);
    if (auto it = funcInstances_.find(key); it != funcInstances_.end()) return it->second;
    auto* inst = arena_.make<FuncDecl>(g->loc);
    inst->name = g->name;
    inst->params = g->params;
    inst->returnTypeExpr = g->returnTypeExpr;
    inst->body = static_cast<BlockStmt*>(cloneStmt(arena_, g->body));
    inst->genericOrigin = g;
    inst->typeArgs = args;
    funcInstances_[key] = inst;
    instantiatedAt_[inst] = at;
    pushTypeEnv(g->typeParams, args);
    resolveSignature(inst);
    popTypeEnv();
    pendingInstances_.push_back(inst);
    return inst;
}

// Matches a parameter type pattern against an argument type, binding type parameters.
// A parameter that is already bound is not re-checked here: conversion costs decide later.
bool Sema::unify(TypeExpr* p, Type* a, const std::vector<std::string>& params, std::map<std::string, Type*>& env) {
    if (!a) return false;
    switch (p->kind) {
    case TypeExpr::Kind::Named:
        if (p->typeArgs.empty() && std::find(params.begin(), params.end(), p->name) != params.end()) {
            if (!env.count(p->name)) env[p->name] = a;
            return true;
        }
        if (!p->typeArgs.empty() && a->isStruct() && a->structDecl->genericOrigin &&
            a->structDecl->genericOrigin->name == p->name && a->structDecl->typeArgs.size() == p->typeArgs.size()) {
            for (size_t i = 0; i < p->typeArgs.size(); ++i)
                if (!unify(p->typeArgs[i], a->structDecl->typeArgs[i], params, env)) return false;
        }
        return true;
    case TypeExpr::Kind::Pointer:
        if (a->isPointer() || a->isArray()) return unify(p->inner, a->elem, params, env);
        return true;
    case TypeExpr::Kind::Array:
        if (a->isArray()) return unify(p->inner, a->elem, params, env);
        return true;
    }
    return true;
}

bool Sema::deduceTypeArgs(FuncDecl* f, const std::vector<Expr*>& args, const std::vector<Type*>* explicitArgs,
                          std::vector<Type*>& out) {
    std::map<std::string, Type*> env;
    if (explicitArgs) {
        if (explicitArgs->size() > f->typeParams.size()) return false;
        for (size_t i = 0; i < explicitArgs->size(); ++i) env[f->typeParams[i]] = (*explicitArgs)[i];
    }
    auto isBareParam = [&](TypeExpr* t) {
        return t->kind == TypeExpr::Kind::Named && t->typeArgs.empty() &&
               std::find(f->typeParams.begin(), f->typeParams.end(), t->name) != f->typeParams.end();
    };
    // Structured parameters (List<T>*, T*) carry the most reliable information, then
    // plain `T` parameters with typed arguments, then untyped literals.
    for (int pass = 0; pass < 3; ++pass) {
        for (size_t i = 0; i < args.size(); ++i) {
            TypeExpr* p = f->params[i].typeExpr;
            bool bare = isBareParam(p);
            bool literal = isUntyped(args[i]);
            if ((pass == 0 && (bare || literal)) || (pass == 1 && (!bare || literal)) || (pass == 2 && !literal))
                continue;
            if (!unify(p, args[i]->type, f->typeParams, env)) return false;
        }
    }
    out.clear();
    for (const std::string& name : f->typeParams) {
        auto it = env.find(name);
        if (it == env.end()) return false;
        out.push_back(it->second);
    }
    return true;
}

void Sema::resolveEnum(EnumDecl* e) {
    int64_t next = 0;
    std::map<std::string, bool> seen;
    for (EnumMember& m : e->members) {
        if (seen[m.name]) diags_.error(m.loc, "duplicate enum member '" + m.name + "'");
        seen[m.name] = true;
        if (m.value) {
            Expr* v = coerce(check(m.value), types_.i64(), "enum value");
            if (v && v->kind == ExprKind::IntLit) {
                next = static_cast<int64_t>(v->as<IntLitExpr>()->value);
            } else if (v && v->type) {
                diags_.error(m.loc, "enum value must be a constant integer");
            }
        }
        m.resolved = next++;
    }
}

void Sema::resolveGlobal(GlobalVar* g) {
    int& state = globalState_[g];
    if (state == 2) return;
    if (state == 1) {
        diags_.error(g->loc, "constant '" + g->name + "' depends on itself");
        return;
    }
    state = 1;
    if (g->typeExpr) {
        g->type = resolveType(g->typeExpr);
    } else {
        g->init = check(g->init);
        g->type = inferType(g->init);
    }
    if (g->type && g->type->isVoid()) {
        diags_.error(g->loc, "a variable cannot have type void");
        g->type = nullptr;
    }
    if (g->type && g->init) {
        Expr* v = coerce(g->typeExpr ? check(g->init) : g->init, g->type, "initializer");
        g->init = v;
        if (v && v->type) {
            if (v->kind == ExprKind::IntLit) {
                g->hasValue = true;
                g->intValue = v->as<IntLitExpr>()->value;
            } else if (v->kind == ExprKind::FloatLit) {
                g->hasValue = true;
                g->isFloatValue = true;
                g->floatValue = v->as<FloatLitExpr>()->value;
            } else if (v->kind == ExprKind::BoolLit) {
                g->hasValue = true;
                g->intValue = v->as<BoolLitExpr>()->value;
            } else if (v->kind == ExprKind::NullLit) {
                g->hasValue = true;
                g->intValue = 0;
            } else {
                diags_.error(g->init->loc, g->isConst ? "a const value must be a compile-time constant"
                                                      : "a global initializer must be a compile-time constant "
                                                        "(initialize it in main instead)");
            }
        }
    }
    if (g->isConst && g->type && !g->type->isScalar() && !g->type->isFloat())
        diags_.error(g->loc, "a const must have a number, bool or pointer type");
    if (!g->isConst && g->type) globals.push_back(g);
    state = 2;
}

void Sema::resolveSignature(FuncDecl* f) {
    f->returnType = f->returnTypeExpr ? resolveType(f->returnTypeExpr) : types_.voidType();
    for (Param& p : f->params) {
        Type* t = resolveType(p.typeExpr);
        if (t && t->isVoid()) {
            diags_.error(p.loc, "a parameter cannot have type void");
            t = nullptr;
        }
        if (t && t->isArray()) {
            diags_.error(p.loc, "arrays cannot be passed by value; pass a pointer (" +
                                    typeName(t->elem) + "*) or wrap the array in a struct");
            t = nullptr;
        }
        f->paramTypes.push_back(t);
    }
    if (f->returnType && f->returnType->isArray()) {
        diags_.error(f->loc, "functions cannot return arrays; wrap the array in a struct");
        f->returnType = nullptr;
    }

    bool isOperator = f->name.size() > 2 && f->name.rfind("op", 0) == 0 &&
                      !std::isalpha(static_cast<unsigned char>(f->name[2])) && f->name[2] != '_';
    if (isOperator) {
        size_t n = f->params.size();
        bool unaryOk = n == 1 && f->name == "op-";
        if (n != 2 && !unaryOk)
            diags_.error(f->loc, "'" + displayName(f->name) + "' must take 2 parameters" +
                                     (f->name == "op-" ? " (or 1 for unary minus)" : ""));
        bool hasStruct = false;
        for (Type* t : f->paramTypes) hasStruct |= t && t->isStruct();
        if (!hasStruct) diags_.error(f->loc, "operator overloads need at least one struct parameter");
    }
}

// ───────────────────────────── Scopes ─────────────────────────────

LocalVar* Sema::lookupLocal(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return nullptr;
}

LocalVar* Sema::declareLocal(const std::string& name, Type* type, SourceLoc loc, bool isConst) {
    if (scopes_.back().count(name)) diags_.error(loc, "'" + name + "' is already declared in this scope");
    localStorage_.push_back(std::make_unique<LocalVar>());
    LocalVar* v = localStorage_.back().get();
    v->name = name;
    v->type = type;
    v->isConst = isConst;
    scopes_.back()[name] = v;
    return v;
}

// ───────────────────────────── Functions & statements ─────────────────────────────

void Sema::checkFunction(FuncDecl* f) {
    currentFunc_ = f;
    pushScope();
    for (size_t i = 0; i < f->params.size(); ++i) {
        LocalVar* v = declareLocal(f->params[i].name, f->paramTypes[i], f->params[i].loc, false);
        v->isParam = true;
        f->paramVars.push_back(v);
    }
    checkBlock(f->body, false);
    popScope();

    bool isMain = f->name == "main" && f->params.empty();
    if (f->returnType && !f->returnType->isVoid() && !isMain && !alwaysReturns(f->body))
        diags_.error(f->loc, "function '" + f->name + "' does not return a value on every path");
    currentFunc_ = nullptr;
}

bool Sema::alwaysReturns(Stmt* s) {
    if (!s) return false;
    switch (s->kind) {
    case StmtKind::Return:
        return true;
    case StmtKind::Block:
        for (Stmt* c : s->as<BlockStmt>()->stmts)
            if (alwaysReturns(c)) return true;
        return false;
    case StmtKind::If: {
        auto* i = s->as<IfStmt>();
        return i->elseStmt && alwaysReturns(i->thenStmt) && alwaysReturns(i->elseStmt);
    }
    case StmtKind::While: {
        // `while (true)` never falls through (a `break` would, but we accept that approximation)
        Expr* c = s->as<WhileStmt>()->cond;
        return c && c->kind == ExprKind::BoolLit && c->as<BoolLitExpr>()->value;
    }
    case StmtKind::For:
        return s->as<ForStmt>()->cond == nullptr;
    case StmtKind::DoWhile:
        return alwaysReturns(s->as<DoWhileStmt>()->body);
    case StmtKind::Switch: {
        // Every case returns, and the switch cannot fall out (default, or all enum members).
        auto* sw = s->as<SwitchStmt>();
        bool covered = false;
        for (const SwitchCase& c : sw->cases) {
            if (c.isDefault) covered = true;
            bool returns = false;
            for (Stmt* st : c.body) returns |= alwaysReturns(st);
            if (!returns) return false;
        }
        Type* t = sw->subject->type;
        if (!covered && t && t->enumDecl) {
            size_t values = 0;
            for (const SwitchCase& c : sw->cases) values += c.values.size();
            covered = values >= t->enumDecl->members.size();
        }
        return covered;
    }
    default:
        return false;
    }
}

// Type of `var x = e;`: untyped literals default to int / double.
Type* Sema::inferType(Expr*& e) {
    if (!e || !e->type) return nullptr;
    if (e->type->isNull()) {
        diags_.error(e->loc, "cannot infer a type from 'null'; write the pointer type (e.g. Enemy* p = null;)");
        return nullptr;
    }
    if (e->type->isVoid()) {
        diags_.error(e->loc, "this expression has no value");
        return nullptr;
    }
    if (e->kind == ExprKind::ArrayLit) {
        e = coerce(e, e->type, "array literal");
        return e->type;
    }
    if (isUntypedFloat(e)) {
        e = foldCast(e, types_.f64(), false);
        return types_.f64();
    }
    if (isUntypedInt(e)) {
        Type* t = e->type;
        e = foldCast(e, t, false);
        return t;
    }
    return e->type;
}

// Finds the loop/switch a break or continue refers to; returns its index or -1.
int Sema::findBreakTarget(const std::string& label, bool isContinue, SourceLoc loc) {
    for (int i = static_cast<int>(breakTargets_.size()) - 1; i >= 0; --i) {
        const BreakTarget& t = breakTargets_[i];
        if (t.kind == BreakTarget::Barrier) break;
        if (!label.empty()) {
            if (t.kind == BreakTarget::Loop && t.label == label) return i;
            continue;
        }
        if (t.kind == BreakTarget::Loop || (!isContinue && t.kind == BreakTarget::Switch)) return i;
    }
    const char* what = isContinue ? "continue" : "break";
    if (!label.empty()) diags_.error(loc, std::string("no enclosing loop labeled '") + label + "' for '" + what + "'");
    else if (!breakTargets_.empty() && breakTargets_.back().kind == BreakTarget::Barrier)
        diags_.error(loc, std::string("'") + what + "' cannot leave a defer block");
    else diags_.error(loc, std::string("'") + what + "' outside of a loop" + (isContinue ? "" : " or switch"));
    return -1;
}

void Sema::checkLoopBody(Stmt* body, const std::string& label) {
    if (body->kind == StmtKind::Defer) diags_.error(body->loc, "defer must be inside a block { ... }");
    breakTargets_.push_back({label, BreakTarget::Loop});
    ++loopDepth_;
    pushScope();
    checkStmt(body);
    popScope();
    --loopDepth_;
    breakTargets_.pop_back();
}

void Sema::checkBlock(BlockStmt* b, bool newScope) {
    if (newScope) pushScope();
    for (Stmt* s : b->stmts) checkStmt(s);
    if (newScope) popScope();
}

Expr* Sema::checkCondition(Expr* e) {
    e = check(e);
    if (!e->type) return e;
    if (!e->type->isBool()) {
        diags_.error(e->loc, "condition must be a bool, not '" + typeName(e->type) + "'" +
                                 (e->type->isNumeric() || e->type->isPointer() ? " (compare explicitly, e.g. x != 0)" : ""));
    }
    return e;
}

void Sema::checkStmt(Stmt* s) {
    switch (s->kind) {
    case StmtKind::Block:
        checkBlock(s->as<BlockStmt>());
        break;
    case StmtKind::VarDecl: {
        auto* v = s->as<VarDeclStmt>();
        Type* t = nullptr;
        if (v->typeExpr) {
            t = resolveType(v->typeExpr);
            if (t && t->isVoid()) {
                diags_.error(v->loc, "a variable cannot have type void");
                t = nullptr;
            }
            if (v->init) {
                v->init = check(v->init);
                if (t) v->init = coerce(v->init, t, "initializer");
            }
        } else {
            v->init = check(v->init);
            t = inferType(v->init);
        }
        v->var = declareLocal(v->name, t, v->loc, v->isConst);
        currentFunc_->locals.push_back(v->var);
        break;
    }
    case StmtKind::Expr: {
        auto* e = s->as<ExprStmt>();
        e->expr = check(e->expr);
        break;
    }
    case StmtKind::If: {
        auto* i = s->as<IfStmt>();
        i->cond = checkCondition(i->cond);
        if (i->thenStmt->kind == StmtKind::Defer || (i->elseStmt && i->elseStmt->kind == StmtKind::Defer))
            diags_.error(s->loc, "defer must be inside a block { ... }");
        pushScope();
        checkStmt(i->thenStmt);
        popScope();
        if (i->elseStmt) {
            pushScope();
            checkStmt(i->elseStmt);
            popScope();
        }
        break;
    }
    case StmtKind::While: {
        auto* w = s->as<WhileStmt>();
        w->cond = checkCondition(w->cond);
        checkLoopBody(w->body, w->label);
        break;
    }
    case StmtKind::DoWhile: {
        auto* d = s->as<DoWhileStmt>();
        checkLoopBody(d->body, d->label);
        d->cond = checkCondition(d->cond);
        break;
    }
    case StmtKind::For: {
        auto* f = s->as<ForStmt>();
        pushScope();
        if (f->init) checkStmt(f->init);
        if (f->cond) f->cond = checkCondition(f->cond);
        if (f->step) f->step = check(f->step);
        checkLoopBody(f->body, f->label);
        popScope();
        break;
    }
    case StmtKind::Switch: {
        auto* sw = s->as<SwitchStmt>();
        sw->subject = check(sw->subject);
        Type* t = sw->subject->type;
        if (t && !t->isInt()) {
            diags_.error(sw->subject->loc, "switch needs an integer or enum value, not '" + typeName(t) + "'");
            t = nullptr;
        }
        if (t && isUntypedInt(sw->subject)) sw->subject = foldCast(sw->subject, t, false);
        std::map<uint64_t, bool> seen;
        bool hasDefault = false;
        breakTargets_.push_back({"", BreakTarget::Switch});
        for (SwitchCase& c : sw->cases) {
            if (c.isDefault) {
                if (hasDefault) diags_.error(c.loc, "duplicate 'default' in switch");
                hasDefault = true;
            }
            for (Expr*& v : c.values) {
                v = check(v);
                if (!t || !v->type) continue;
                v = coerce(v, t, "case value");
                if (!v->type) continue;
                if (v->kind != ExprKind::IntLit) {
                    diags_.error(v->loc, "a case value must be a constant");
                    continue;
                }
                uint64_t key = v->as<IntLitExpr>()->value;
                if (seen[key]) diags_.error(v->loc, "duplicate case value");
                seen[key] = true;
            }
            pushScope();
            for (Stmt* st : c.body) checkStmt(st);
            popScope();
        }
        breakTargets_.pop_back();
        if (t && t->enumDecl && !hasDefault) {
            std::string missing;
            for (const EnumMember& m : t->enumDecl->members)
                if (!seen.count(normalizeInt(static_cast<uint64_t>(m.resolved), t))) missing += (missing.empty() ? "" : ", ") + m.name;
            if (!missing.empty())
                diags_.error(sw->loc, "switch on '" + t->name + "' does not handle: " + missing +
                                          " (add these cases or a default)");
        }
        break;
    }
    case StmtKind::Defer: {
        auto* d = s->as<DeferStmt>();
        breakTargets_.push_back({"", BreakTarget::Barrier});
        ++deferDepth_;
        checkStmt(d->body);
        --deferDepth_;
        breakTargets_.pop_back();
        break;
    }
    case StmtKind::Return: {
        auto* r = s->as<ReturnStmt>();
        Type* rt = currentFunc_->returnType;
        if (deferDepth_ > 0) diags_.error(r->loc, "'return' is not allowed inside a defer block");
        if (r->value) {
            r->value = check(r->value);
            if (rt && rt->isVoid()) {
                diags_.error(r->loc, "function '" + currentFunc_->name + "' returns nothing, but a value is returned");
            } else if (rt) {
                r->value = coerce(r->value, rt, "return value");
            }
        } else if (rt && !rt->isVoid() && !(currentFunc_->name == "main")) {
            diags_.error(r->loc, "missing return value (function returns '" + typeName(rt) + "')");
        }
        break;
    }
    case StmtKind::Break:
        findBreakTarget(s->as<BreakStmt>()->label, false, s->loc);
        break;
    case StmtKind::Continue:
        findBreakTarget(s->as<ContinueStmt>()->label, true, s->loc);
        break;
    }
}

// ───────────────────────────── Conversions ─────────────────────────────

void Sema::setUntypedIntType(IntLitExpr* e) {
    if (literalFits(e, types_.i32())) e->type = types_.i32();
    else if (literalFits(e, types_.i64())) e->type = types_.i64();
    else e->type = types_.u64();
}

// -1 = impossible, 0 = exact, larger = worse. Used for overload resolution.
int Sema::conversionCost(Expr* e, Type* to) {
    Type* from = e->type;
    if (!from || !to) return -1;
    if (e->kind == ExprKind::ArrayLit && to->isArray()) {
        auto* a = e->as<ArrayLitExpr>();
        if (a->elems.size() > to->count) return -1;
        int worst = 0;
        for (Expr* x : a->elems) {
            int c = conversionCost(x, to->elem);
            if (c < 0) return -1;
            worst = std::max(worst, c);
        }
        return worst;
    }
    if (e->kind == ExprKind::Ternary && isUntyped(e)) {
        int a = conversionCost(e->as<TernaryExpr>()->thenExpr, to);
        int b = conversionCost(e->as<TernaryExpr>()->elseExpr, to);
        return a < 0 || b < 0 ? -1 : std::max(a, b);
    }
    if (from == to) return isUntyped(e) ? 1 : 0;
    if (isUntypedInt(e)) {
        // Prefer signed, then unsigned, then float overloads for integer literals.
        if (to->isInt() && !to->enumDecl) return literalFits(e->as<IntLitExpr>(), to) ? (to->isSigned ? 1 : 2) : -1;
        if (to->isFloat()) return 3;
        return -1;
    }
    if (isUntypedFloat(e)) return to->isFloat() ? 1 : -1;
    if (from->isNull()) return to->isPointer() ? 1 : -1;
    if (from->isInt() && to->isInt()) {
        if (from->enumDecl) {
            // enums convert implicitly to integers wide enough for i32
            return (to->isSigned && to->size >= 4) || to->size > 4 ? 3 : -1;
        }
        if (to->enumDecl) return -1;
        if (from->size < to->size && (from->isSigned == to->isSigned || (!from->isSigned && to->isSigned))) {
            // The closest wider type wins (u8 prefers u16 over u64), same signedness first.
            int steps = 0;
            for (uint32_t s = from->size; s < to->size; s *= 2) ++steps;
            return 2 + steps + (from->isSigned != to->isSigned ? 1 : 0);
        }
        return -1;
    }
    if (from->isInt() && to->isFloat()) return 8;
    if (from->isFloat() && to->isFloat()) return from->size < to->size ? 2 : -1;
    if (from->isPointer() && to->isPointer()) {
        if (to->isVoidPointer() || from->isVoidPointer()) return 2;
        return -1;
    }
    if (from->isArray() && to->isPointer() && (to->elem == from->elem || to->isVoidPointer())) return 2;
    return -1;
}

Expr* Sema::implicitCast(Expr* e, Type* to) {
    auto* c = arena_.make<CastExpr>(e->loc);
    c->operand = e;
    c->type = to;
    return c;
}

// Converts a literal to another type at compile time, or wraps it in a cast.
Expr* Sema::foldCast(Expr* e, Type* to, bool explicitCast) {
    if (e->kind == ExprKind::Ternary && isUntyped(e)) {
        auto* t = e->as<TernaryExpr>();
        t->thenExpr = foldCast(t->thenExpr, to, explicitCast);
        t->elseExpr = foldCast(t->elseExpr, to, explicitCast);
        t->type = to;
        return t;
    }
    if (e->kind == ExprKind::IntLit) {
        auto* lit = e->as<IntLitExpr>();
        if (to->isInt()) {
            lit->value = normalizeInt(lit->value, to);
            lit->negative = to->isSigned && static_cast<int64_t>(lit->value) < 0;
            lit->untyped = false;
            lit->type = to;
            return lit;
        }
        if (to->isFloat()) {
            auto* f = arena_.make<FloatLitExpr>(e->loc);
            bool isSigned = lit->negative || (lit->type && lit->type->isSigned);
            f->value = isSigned ? static_cast<double>(static_cast<int64_t>(lit->value))
                                : static_cast<double>(lit->value);
            f->untyped = false;
            f->type = to;
            return f;
        }
        if (to->isBool() && explicitCast) {
            auto* b = arena_.make<BoolLitExpr>(e->loc);
            b->value = lit->value != 0;
            b->type = to;
            return b;
        }
    }
    if (e->kind == ExprKind::FloatLit) {
        auto* lit = e->as<FloatLitExpr>();
        if (to->isFloat()) {
            lit->value = to->size == 4 ? static_cast<double>(static_cast<float>(lit->value)) : lit->value;
            lit->untyped = false;
            lit->type = to;
            return lit;
        }
        if (to->isInt() && explicitCast && std::isfinite(lit->value)) {
            auto* i = arena_.make<IntLitExpr>(e->loc);
            i->value = to->isSigned ? static_cast<uint64_t>(static_cast<int64_t>(lit->value))
                                    : static_cast<uint64_t>(lit->value);
            i->value = normalizeInt(i->value, to);
            i->negative = to->isSigned && static_cast<int64_t>(i->value) < 0;
            i->untyped = false;
            i->type = to;
            return i;
        }
    }
    if (e->kind == ExprKind::NullLit && to->isPointer()) {
        e->type = to;
        return e;
    }
    return implicitCast(e, to);
}

Expr* Sema::coerce(Expr* e, Type* to, const char* context) {
    if (!e || !e->type || !to) return e;
    if (e->kind == ExprKind::ArrayLit && to->isArray()) {
        auto* a = e->as<ArrayLitExpr>();
        if (a->elems.size() > to->count) {
            diags_.error(e->loc, "array literal has " + std::to_string(a->elems.size()) + " elements, but '" +
                                     typeName(to) + "' holds " + std::to_string(to->count));
            return errorExpr(e);
        }
        for (Expr*& x : a->elems) {
            x = coerce(x, to->elem, "array element");
            if (!x->type) return errorExpr(e);
        }
        a->type = to;
        return a;
    }
    if (e->type == to && !isUntyped(e)) return e;
    if (to->isStruct() && e->type == to) return e;
    int cost = conversionCost(e, to);
    if (cost < 0) {
        std::string msg = std::string("cannot convert '") + typeName(e->type) + "' to '" + typeName(to) + "' in " + context;
        if (isUntypedInt(e) && to->isInt()) msg = "integer literal does not fit in '" + typeName(to) + "'";
        else if (isUntypedFloat(e) && to->isInt())
            msg = "cannot use a float literal where '" + typeName(to) + "' is expected";
        else if (e->type->isNumeric() && to->isNumeric())
            msg += " (use an explicit cast: 'as " + typeName(to) + "')";
        diags_.error(e->loc, msg);
        return errorExpr(e);
    }
    return foldCast(e, to, false);
}

// Widens any integer used as an index / pointer offset to 64 bits.
Expr* Sema::toOffset(Expr* e) {
    if (isUntypedInt(e)) return foldCast(e, types_.i64(), false);
    if (e->type->size < 8 || e->type->enumDecl)
        return implicitCast(e, e->type->isSigned ? types_.i64() : types_.u64());
    return e;
}

bool Sema::castAllowed(Type* from, Type* to) {
    if (from == to) return true;
    if ((from->isNumeric() || from->isBool()) && to->isNumeric()) return true;
    if (from->isNumeric() && to->isBool()) return true;
    if (from->isPointer() && to->isPointer()) return true;
    if (from->isNull() && to->isPointer()) return true;
    if (from->isPointer() && to->isInt() && to->size == 8) return true;
    if (from->isInt() && from->size == 8 && to->isPointer()) return true;
    if (from->isArray() && to->isPointer()) return true;
    return false;
}

// ───────────────────────────── Overloads ─────────────────────────────

FuncDecl* Sema::resolveOverload(const std::string& name, std::vector<Expr*>& args, SourceLoc loc,
                                bool reportErrors, Expr* ufcsBase, const std::vector<Type*>* explicitTypeArgs) {
    auto it = funcs_.find(name);
    if (it == funcs_.end()) {
        if (reportErrors) diags_.error(loc, "unknown function '" + displayName(name) + "'");
        return nullptr;
    }

    // With UFCS, `base.f(x)` may also bind `&base` to a pointer first parameter.
    Expr* addrOfBase = nullptr;
    if (ufcsBase && ufcsBase->isLvalue && ufcsBase->type) {
        auto* u = arena_.make<UnaryExpr>(ufcsBase->loc);
        u->op = UnaryOp::AddrOf;
        u->operand = ufcsBase;
        u->type = types_.pointerTo(ufcsBase->type);
        addrOfBase = u;
    }

    bool hasExplicit = explicitTypeArgs && !explicitTypeArgs->empty();
    FuncDecl* best = nullptr;
    std::vector<Type*> bestTypeArgs;
    bool bestUsesAddr = false;
    int bestCost = std::numeric_limits<int>::max();
    int ties = 0;
    for (FuncDecl* f : it->second) {
        if (f->params.size() != args.size()) continue;
        bool isTemplate = !f->typeParams.empty();
        if (hasExplicit && !isTemplate) continue;
        for (int variant = 0; variant < (addrOfBase ? 2 : 1); ++variant) {
            std::vector<Expr*> actual = args;
            if (variant == 1) actual[0] = addrOfBase;
            std::vector<Type*> paramTypes = f->paramTypes;
            std::vector<Type*> typeArgs;
            if (isTemplate) {
                if (!deduceTypeArgs(f, actual, explicitTypeArgs, typeArgs)) continue;
                pushTypeEnv(f->typeParams, typeArgs);
                paramTypes.clear();
                bool resolved = true;
                for (const Param& p : f->params) {
                    Type* t = resolveType(p.typeExpr);
                    resolved &= t != nullptr;
                    paramTypes.push_back(t);
                }
                popTypeEnv();
                if (!resolved) continue;
            }
            int total = 0;
            bool ok = true;
            for (size_t i = 0; i < actual.size() && ok; ++i) {
                int c = conversionCost(actual[i], paramTypes[i]);
                if (c < 0) ok = false;
                total += c;
            }
            if (!ok) continue;
            // A non-template wins over a template with equally good conversions.
            total = total * 2 + (isTemplate ? 1 : 0);
            if (total < bestCost) {
                best = f;
                bestTypeArgs = typeArgs;
                bestCost = total;
                bestUsesAddr = variant == 1;
                ties = 0;
            } else if (total == bestCost && f != best) {
                ++ties;
            }
        }
    }

    if (!best) {
        if (reportErrors) {
            std::string sig = displayName(name) + "(";
            for (size_t i = 0; i < args.size(); ++i) sig += (i ? ", " : "") + typeName(args[i]->type);
            sig += ")";
            diags_.error(loc, "no matching function for call to " + sig);
            for (FuncDecl* f : it->second) {
                std::string cand = displayName(name);
                if (!f->typeParams.empty()) {
                    cand += "<";
                    for (size_t i = 0; i < f->typeParams.size(); ++i) cand += (i ? ", " : "") + f->typeParams[i];
                    cand += ">";
                }
                cand += "(";
                for (size_t i = 0; i < f->params.size(); ++i) {
                    TypeExpr* t = f->params[i].typeExpr;
                    std::string tn = i < f->paramTypes.size() && f->paramTypes[i] ? typeName(f->paramTypes[i])
                                                                                  : t->name + (t->kind == TypeExpr::Kind::Pointer ? "*" : "");
                    cand += (i ? ", " : "") + tn;
                }
                diags_.note(f->loc, "candidate: " + cand + ")");
            }
        }
        return nullptr;
    }
    if (ties > 0 && reportErrors) {
        diags_.error(loc, "call to '" + displayName(name) + "' is ambiguous");
        return nullptr;
    }

    if (bestUsesAddr) {
        args[0] = addrOfBase;
        if (ufcsBase->kind == ExprKind::Ident && ufcsBase->as<IdentExpr>()->local)
            ufcsBase->as<IdentExpr>()->local->addressTaken = true;
    }
    if (!best->typeParams.empty()) best = instantiateFunction(best, bestTypeArgs, loc);
    for (size_t i = 0; i < args.size(); ++i) {
        if (!best->paramTypes[i]) return nullptr;
        args[i] = coerce(args[i], best->paramTypes[i], "argument");
    }
    if (currentFunc_) currentFunc_->callees.push_back(best);
    return best;
}

// ───────────────────────────── Expressions ─────────────────────────────

Expr* Sema::check(Expr* e) {
    switch (e->kind) {
    case ExprKind::IntLit: {
        auto* lit = e->as<IntLitExpr>();
        if (lit->untyped) setUntypedIntType(lit);
        return e;
    }
    case ExprKind::FloatLit: {
        auto* lit = e->as<FloatLitExpr>();
        e->type = lit->untyped ? types_.f64() : types_.f32();
        return e;
    }
    case ExprKind::StringLit:
        e->type = types_.pointerTo(types_.u8());
        return e;
    case ExprKind::BoolLit:
        e->type = types_.boolType();
        return e;
    case ExprKind::NullLit:
        e->type = types_.nullType();
        return e;
    case ExprKind::Ident: return checkIdent(e->as<IdentExpr>());
    case ExprKind::Path: return checkPath(e->as<PathExpr>());
    case ExprKind::Unary: return checkUnary(e->as<UnaryExpr>());
    case ExprKind::Binary: return checkBinary(e->as<BinaryExpr>());
    case ExprKind::Assign: return checkAssign(e->as<AssignExpr>());
    case ExprKind::Ternary: return checkTernary(e->as<TernaryExpr>());
    case ExprKind::Call: return checkCall(e->as<CallExpr>());
    case ExprKind::Index: return checkIndex(e->as<IndexExpr>());
    case ExprKind::Member: return checkMember(e->as<MemberExpr>());
    case ExprKind::Cast: return checkCast(e->as<CastExpr>());
    case ExprKind::Sizeof: {
        auto* s = e->as<SizeofExpr>();
        Type* t = resolveType(s->target);
        if (!t) return errorExpr(e);
        auto* lit = arena_.make<IntLitExpr>(e->loc);
        lit->value = t->size;
        setUntypedIntType(lit);
        return lit;
    }
    case ExprKind::StructLit: return checkStructLit(e->as<StructLitExpr>());
    case ExprKind::ArrayLit: return checkArrayLit(e->as<ArrayLitExpr>());
    }
    return errorExpr(e);
}

// `[a, b, c]` gets a provisional type T[n]; coerce() adapts it to the expected array type.
Expr* Sema::checkArrayLit(ArrayLitExpr* e) {
    if (e->elems.empty()) {
        diags_.error(e->loc, "an array literal needs at least one element");
        return errorExpr(e);
    }
    for (Expr*& x : e->elems) {
        x = check(x);
        if (!x->type) return errorExpr(e);
    }
    Type* elem = nullptr;
    for (Expr* x : e->elems)
        if (!isUntyped(x)) {
            elem = x->type;
            break;
        }
    if (!elem) {
        bool anyFloat = false;
        Type* widest = e->elems[0]->type;
        for (Expr* x : e->elems) {
            anyFloat |= isUntypedFloat(x);
            if (x->type->size > widest->size) widest = x->type;
        }
        elem = anyFloat ? types_.f64() : widest;
    }
    if (elem->isVoid() || elem->isNull()) {
        diags_.error(e->loc, "cannot infer the element type of this array literal");
        return errorExpr(e);
    }
    e->type = types_.arrayOf(elem, e->elems.size());
    return e;
}

Expr* Sema::checkIdent(IdentExpr* e) {
    if (!e->typeArgs.empty()) {
        diags_.error(e->loc, "type arguments (::<...>) are only allowed on function calls and struct literals");
        return errorExpr(e);
    }
    if (LocalVar* v = lookupLocal(e->name)) {
        double weight = 1;
        for (int i = 0; i < loopDepth_ && i < 6; ++i) weight *= 8;
        v->useWeight += weight;
        e->local = v;
        e->type = v->type;
        e->isLvalue = true;
        return e;
    }
    if (auto it = globalsByName_.find(e->name); it != globalsByName_.end()) {
        GlobalVar* g = it->second;
        resolveGlobal(g);
        if (!g->type) return errorExpr(e);
        if (g->isConst) {
            if (!g->hasValue) return errorExpr(e);
            // Substitute the constant's value.
            if (g->isFloatValue) {
                auto* f = arena_.make<FloatLitExpr>(e->loc);
                f->value = g->floatValue;
                f->untyped = false;
                f->type = g->type;
                return f;
            }
            if (g->type->isBool()) {
                auto* b = arena_.make<BoolLitExpr>(e->loc);
                b->value = g->intValue != 0;
                b->type = g->type;
                return b;
            }
            if (g->type->isPointer()) {
                auto* n = arena_.make<NullLitExpr>(e->loc);
                n->type = g->type;
                return n;
            }
            auto* i = arena_.make<IntLitExpr>(e->loc);
            i->value = g->intValue;
            i->untyped = false;
            i->negative = g->type->isSigned && static_cast<int64_t>(g->intValue) < 0;
            i->type = g->type;
            return i;
        }
        e->global = g;
        e->type = g->type;
        e->isLvalue = true;
        return e;
    }
    if (funcs_.count(e->name)) {
        diags_.error(e->loc, "'" + e->name + "' is a function; call it with ()");
        return errorExpr(e);
    }
    diags_.error(e->loc, "unknown name '" + e->name + "'");
    return errorExpr(e);
}

Expr* Sema::checkPath(PathExpr* e) {
    auto it = enums_.find(e->scope);
    if (it == enums_.end()) {
        diags_.error(e->loc, "unknown enum '" + e->scope + "'");
        return errorExpr(e);
    }
    for (const EnumMember& m : it->second->members) {
        if (m.name == e->member) {
            auto* lit = arena_.make<IntLitExpr>(e->loc);
            lit->value = static_cast<uint64_t>(m.resolved);
            lit->negative = m.resolved < 0;
            lit->untyped = false;
            lit->type = it->second->type;
            return lit;
        }
    }
    diags_.error(e->loc, "enum '" + e->scope + "' has no member '" + e->member + "'");
    return errorExpr(e);
}

Expr* Sema::checkUnary(UnaryExpr* e) {
    e->operand = check(e->operand);
    Expr* x = e->operand;
    if (!x->type) return errorExpr(e);
    Type* t = x->type;

    switch (e->op) {
    case UnaryOp::Neg:
        if (x->kind == ExprKind::IntLit) {
            auto* lit = x->as<IntLitExpr>();
            if (lit->untyped) {
                int64_t v;
                if (!literalAsInt64(lit, v) && !(lit->value == uint64_t(1) << 63)) {
                    diags_.error(e->loc, "integer literal too large to negate");
                    return errorExpr(e);
                }
                lit->value = static_cast<uint64_t>(-static_cast<int64_t>(lit->value));
                lit->negative = static_cast<int64_t>(lit->value) < 0;
                setUntypedIntType(lit);
                lit->loc = e->loc;
                return lit;
            }
            if (t->isSigned) {
                lit->value = normalizeInt(static_cast<uint64_t>(-static_cast<int64_t>(lit->value)), t);
                lit->negative = static_cast<int64_t>(lit->value) < 0;
                return lit;
            }
        }
        if (x->kind == ExprKind::FloatLit) {
            x->as<FloatLitExpr>()->value = -x->as<FloatLitExpr>()->value;
            x->loc = e->loc;
            return x;
        }
        if (t->isStruct()) {
            std::vector<Expr*> args{x};
            e->overload = resolveOverload("op-", args, e->loc, true);
            if (!e->overload) return errorExpr(e);
            e->operand = args[0];
            e->type = e->overload->returnType;
            return e;
        }
        if (!t->isNumeric() || (t->isInt() && !t->isSigned)) {
            diags_.error(e->loc, "cannot negate a value of type '" + typeName(t) + "'");
            return errorExpr(e);
        }
        e->type = t;
        return e;
    case UnaryOp::Not:
        if (!t->isBool()) {
            diags_.error(e->loc, "'!' needs a bool, not '" + typeName(t) + "'");
            return errorExpr(e);
        }
        if (x->kind == ExprKind::BoolLit) {
            x->as<BoolLitExpr>()->value = !x->as<BoolLitExpr>()->value;
            return x;
        }
        e->type = t;
        return e;
    case UnaryOp::BitNot:
        if (!t->isInt()) {
            diags_.error(e->loc, "'~' needs an integer, not '" + typeName(t) + "'");
            return errorExpr(e);
        }
        if (x->kind == ExprKind::IntLit && !isUntypedInt(x)) {
            auto* lit = x->as<IntLitExpr>();
            lit->value = normalizeInt(~lit->value, t);
            lit->negative = t->isSigned && static_cast<int64_t>(lit->value) < 0;
            return lit;
        }
        if (isUntypedInt(x)) x = e->operand = foldCast(x, x->type, false);
        e->type = x->type;
        return e;
    case UnaryOp::AddrOf:
        if (!x->isLvalue) {
            diags_.error(e->loc, "cannot take the address of a temporary value");
            return errorExpr(e);
        }
        if (x->kind == ExprKind::Ident && x->as<IdentExpr>()->local) x->as<IdentExpr>()->local->addressTaken = true;
        e->type = types_.pointerTo(t);
        return e;
    case UnaryOp::Deref:
        if (!t->isPointer() || t->isVoidPointer()) {
            diags_.error(e->loc, "cannot dereference a value of type '" + typeName(t) + "'");
            return errorExpr(e);
        }
        e->type = t->elem;
        e->isLvalue = true;
        return e;
    case UnaryOp::PreInc:
    case UnaryOp::PreDec:
    case UnaryOp::PostInc:
    case UnaryOp::PostDec:
        if (!x->isLvalue || (x->kind == ExprKind::Ident && x->as<IdentExpr>()->local &&
                             x->as<IdentExpr>()->local->isConst)) {
            diags_.error(e->loc, std::string("'") + unaryOpText(e->op) + "' needs a modifiable variable");
            return errorExpr(e);
        }
        if (!t->isNumeric() || t->enumDecl) {
            if (!(t->isPointer() && !t->isVoidPointer())) {
                diags_.error(e->loc, std::string("cannot apply '") + unaryOpText(e->op) + "' to '" + typeName(t) + "'");
                return errorExpr(e);
            }
        }
        e->type = t;
        return e;
    }
    return errorExpr(e);
}

Expr* Sema::fold(BinaryExpr* e) {
    Expr* l = e->lhs;
    Expr* r = e->rhs;
    if (l->kind == ExprKind::IntLit && r->kind == ExprKind::IntLit) {
        auto* a = l->as<IntLitExpr>();
        auto* b = r->as<IntLitExpr>();
        // A shift keeps the (untyped) kind of its left operand.
        bool untyped = a->untyped && (b->untyped || e->op == BinaryOp::Shl || e->op == BinaryOp::Shr);
        Type* t = e->type;
        if (!t->isInt()) return e;
        uint64_t x = a->value, y = b->value, v;
        bool isSigned = untyped ? true : t->isSigned;
        int64_t sx, sy;
        if (untyped && (!literalAsInt64(a, sx) || !literalAsInt64(b, sy))) return e;
        switch (e->op) {
        case BinaryOp::Add: v = x + y; break;
        case BinaryOp::Sub: v = x - y; break;
        case BinaryOp::Mul: v = x * y; break;
        case BinaryOp::Div:
            if (y == 0) return e;
            v = isSigned ? static_cast<uint64_t>(static_cast<int64_t>(x) / static_cast<int64_t>(y)) : x / y;
            break;
        case BinaryOp::Mod:
            if (y == 0) return e;
            v = isSigned ? static_cast<uint64_t>(static_cast<int64_t>(x) % static_cast<int64_t>(y)) : x % y;
            break;
        case BinaryOp::BitAnd: v = x & y; break;
        case BinaryOp::BitOr: v = x | y; break;
        case BinaryOp::BitXor: v = x ^ y; break;
        case BinaryOp::Shl: if (y >= 64) return e; v = x << y; break;
        case BinaryOp::Shr:
            if (y >= 64) return e;
            v = isSigned ? static_cast<uint64_t>(static_cast<int64_t>(x) >> y) : x >> y;
            break;
        default: return e;
        }
        auto* lit = arena_.make<IntLitExpr>(e->loc);
        if (untyped) {
            lit->value = v;
            lit->negative = static_cast<int64_t>(v) < 0;
            setUntypedIntType(lit);
        } else {
            lit->value = normalizeInt(v, t);
            lit->negative = t->isSigned && static_cast<int64_t>(lit->value) < 0;
            lit->untyped = false;
            lit->type = t;
        }
        return lit;
    }
    if (l->kind == ExprKind::FloatLit && r->kind == ExprKind::FloatLit) {
        double x = l->as<FloatLitExpr>()->value, y = r->as<FloatLitExpr>()->value, v;
        switch (e->op) {
        case BinaryOp::Add: v = x + y; break;
        case BinaryOp::Sub: v = x - y; break;
        case BinaryOp::Mul: v = x * y; break;
        case BinaryOp::Div: v = x / y; break;
        default: return e;
        }
        auto* lit = arena_.make<FloatLitExpr>(e->loc);
        lit->untyped = isUntypedFloat(l) && isUntypedFloat(r);
        lit->type = e->type;
        lit->value = e->type->size == 4 ? static_cast<double>(static_cast<float>(v)) : v;
        return lit;
    }
    return e;
}

Expr* Sema::checkBinary(BinaryExpr* e) {
    e->lhs = check(e->lhs);
    e->rhs = check(e->rhs);
    if (!e->lhs->type || !e->rhs->type) return errorExpr(e);
    Type* lt = e->lhs->type;
    Type* rt = e->rhs->type;
    BinaryOp op = e->op;
    std::string opText = binaryOpText(op);

    // Operator overloading on structs
    if (lt->isStruct() || rt->isStruct()) {
        std::vector<Expr*> args{e->lhs, e->rhs};
        e->overload = resolveOverload(operatorFuncName(op), args, e->loc, false);
        if (!e->overload) {
            diags_.error(e->loc, "no operator '" + opText + "' for '" + typeName(lt) + "' and '" + typeName(rt) +
                                     "' (define: fn " + opText + "(" + typeName(lt) + " a, " + typeName(rt) + " b) -> ...)");
            return errorExpr(e);
        }
        e->lhs = args[0];
        e->rhs = args[1];
        e->type = e->overload->returnType;
        return e;
    }

    if (op == BinaryOp::LogAnd || op == BinaryOp::LogOr) {
        if (!lt->isBool() || !rt->isBool()) {
            diags_.error(e->loc, "'" + opText + "' needs bool operands");
            return errorExpr(e);
        }
        e->type = types_.boolType();
        return e;
    }

    bool isCompare = op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt || op == BinaryOp::Le ||
                     op == BinaryOp::Gt || op == BinaryOp::Ge;

    // Pointers
    if (lt->isPointer() || rt->isPointer() || lt->isNull() || rt->isNull()) {
        if (isCompare) {
            if (lt->isNull() && rt->isPointer()) e->lhs = coerce(e->lhs, rt, "comparison");
            else if (rt->isNull() && lt->isPointer()) e->rhs = coerce(e->rhs, lt, "comparison");
            else if (lt != rt && !(lt->isNull() && rt->isNull()) && !lt->isVoidPointer() && !rt->isVoidPointer()) {
                diags_.error(e->loc, "cannot compare '" + typeName(lt) + "' with '" + typeName(rt) + "'");
                return errorExpr(e);
            }
            e->type = types_.boolType();
            return e;
        }
        if ((op == BinaryOp::Add || op == BinaryOp::Sub) && lt->isPointer() && !lt->isVoidPointer() && rt->isInt()) {
            e->rhs = toOffset(e->rhs);
            e->type = lt;
            return e;
        }
        if (op == BinaryOp::Sub && lt->isPointer() && lt == rt && !lt->isVoidPointer()) {
            e->type = types_.i64();   // element count between two pointers
            return e;
        }
        diags_.error(e->loc, "invalid operands to '" + opText + "': '" + typeName(lt) + "' and '" + typeName(rt) + "'");
        return errorExpr(e);
    }

    // Booleans: == != & | ^
    if (lt->isBool() || rt->isBool()) {
        bool ok = lt->isBool() && rt->isBool() &&
                  (op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::BitAnd || op == BinaryOp::BitOr ||
                   op == BinaryOp::BitXor);
        if (!ok) {
            diags_.error(e->loc, "invalid operands to '" + opText + "': '" + typeName(lt) + "' and '" + typeName(rt) + "'");
            return errorExpr(e);
        }
        e->type = isCompare ? types_.boolType() : types_.boolType();
        return e;
    }

    if (!lt->isNumeric() || !rt->isNumeric()) {
        diags_.error(e->loc, "invalid operands to '" + opText + "': '" + typeName(lt) + "' and '" + typeName(rt) + "'");
        return errorExpr(e);
    }

    // Shifts: the result has the type of the left operand.
    if (op == BinaryOp::Shl || op == BinaryOp::Shr) {
        if (!lt->isInt() || !rt->isInt()) {
            diags_.error(e->loc, "shift operands must be integers");
            return errorExpr(e);
        }
        if (isUntypedInt(e->rhs)) e->rhs = foldCast(e->rhs, types_.u8(), false);
        e->type = lt;
        return isUntypedInt(e->lhs) && e->rhs->kind == ExprKind::IntLit ? fold(e) : e;
    }

    // Find the common type of both operands.
    Expr*& l = e->lhs;
    Expr*& r = e->rhs;
    Type* common = nullptr;
    if (isUntyped(l) && isUntyped(r)) {
        if (isUntypedFloat(l) || isUntypedFloat(r)) {
            common = types_.f64();
            l = foldCast(l, common, false);
            r = foldCast(r, common, false);
            if (l->kind == ExprKind::FloatLit) l->as<FloatLitExpr>()->untyped = true;
            if (r->kind == ExprKind::FloatLit) r->as<FloatLitExpr>()->untyped = true;
        } else {
            common = l->type->size >= r->type->size ? l->type : r->type;
        }
    } else if (isUntyped(l) || isUntyped(r)) {
        Expr*& lit = isUntyped(l) ? l : r;
        Expr*& other = isUntyped(l) ? r : l;
        if (isUntypedFloat(lit) && other->type->isInt()) {
            // `i * 0.5` computes in float (the default game type), not double.
            common = types_.f32();
            other = implicitCast(other, common);
        } else {
            common = other->type;
        }
        lit = coerce(lit, common, ("operand of '" + opText + "'").c_str());
        if (!lit->type) return errorExpr(e);
    } else if (lt == rt) {
        common = lt;
    } else if (conversionCost(l, rt) >= 0) {
        common = rt;
        l = coerce(l, rt, "operand");
    } else if (conversionCost(r, lt) >= 0) {
        common = lt;
        r = coerce(r, lt, "operand");
    } else {
        diags_.error(e->loc, "mismatched operand types for '" + opText + "': '" + typeName(lt) + "' and '" +
                                 typeName(rt) + "' (use 'as' to convert)");
        return errorExpr(e);
    }

    if (common->isFloat() && (op == BinaryOp::Mod || op == BinaryOp::BitAnd || op == BinaryOp::BitOr ||
                              op == BinaryOp::BitXor)) {
        diags_.error(e->loc, "operator '" + opText + "' is not defined for floats");
        return errorExpr(e);
    }
    if (common->enumDecl && !isCompare && op != BinaryOp::BitOr && op != BinaryOp::BitAnd) {
        // arithmetic on enum values yields the underlying integer
        common = types_.i32();
        l = implicitCast(l, common);
        r = implicitCast(r, common);
    }

    if (isCompare) {
        e->type = types_.boolType();
        return e;
    }
    e->type = common;
    return fold(e);
}

Expr* Sema::checkAssign(AssignExpr* e) {
    e->target = check(e->target);
    e->value = check(e->value);
    Expr* t = e->target;
    if (!t->type || !e->value->type) return errorExpr(e);
    if (!t->isLvalue) {
        diags_.error(e->loc, "left side of assignment is not assignable");
        return errorExpr(e);
    }
    if (t->kind == ExprKind::Ident && t->as<IdentExpr>()->local && t->as<IdentExpr>()->local->isConst) {
        diags_.error(e->loc, "cannot assign to const '" + t->as<IdentExpr>()->name + "'");
        return errorExpr(e);
    }
    if (t->type->isArray()) {
        if (!e->compound && e->value->kind == ExprKind::ArrayLit) e->value = coerce(e->value, t->type, "assignment");
        if (!e->value->type) return errorExpr(e);
        if (e->compound || e->value->type != t->type) {
            diags_.error(e->loc, "cannot assign '" + typeName(e->value->type) + "' to '" + typeName(t->type) + "'");
            return errorExpr(e);
        }
        e->type = t->type;
        return e;
    }

    if (e->compound) {
        // Reuse the binary operator rules on (target op value).
        auto* bin = arena_.make<BinaryExpr>(e->loc);
        bin->op = e->op;
        bin->lhs = t;
        bin->rhs = e->value;
        Type* tt = t->type;
        if (tt->isStruct() || e->value->type->isStruct()) {
            Expr* r = checkBinary(bin);
            if (!r->type) return errorExpr(e);
            e->compound = false;   // a = a op b  (target is evaluated twice)
            e->value = coerce(r, tt, "assignment");
        } else if (tt->isPointer()) {
            if (!(e->op == BinaryOp::Add || e->op == BinaryOp::Sub) || !e->value->type->isInt()) {
                diags_.error(e->loc, "invalid compound assignment on a pointer");
                return errorExpr(e);
            }
            e->value = toOffset(e->value);
        } else if (tt->isNumeric() || tt->isBool()) {
            bool shift = e->op == BinaryOp::Shl || e->op == BinaryOp::Shr;
            if (shift) {
                if (!tt->isInt() || !e->value->type->isInt()) {
                    diags_.error(e->loc, "shift operands must be integers");
                    return errorExpr(e);
                }
                if (isUntypedInt(e->value)) e->value = foldCast(e->value, types_.u8(), false);
            } else {
                if (tt->isFloat() && (e->op == BinaryOp::Mod || e->op == BinaryOp::BitAnd ||
                                      e->op == BinaryOp::BitOr || e->op == BinaryOp::BitXor)) {
                    diags_.error(e->loc, std::string("operator '") + binaryOpText(e->op) + "=' is not defined for floats");
                    return errorExpr(e);
                }
                if (tt->isBool() && !(e->op == BinaryOp::BitAnd || e->op == BinaryOp::BitOr || e->op == BinaryOp::BitXor)) {
                    diags_.error(e->loc, "invalid compound assignment on a bool");
                    return errorExpr(e);
                }
                // `f += i` with f float and i int is allowed (int converts to float).
                e->value = coerce(e->value, tt, "compound assignment");
            }
        } else {
            diags_.error(e->loc, "invalid compound assignment on '" + typeName(tt) + "'");
            return errorExpr(e);
        }
        e->type = tt;
        return e;
    }

    e->value = coerce(e->value, t->type, "assignment");
    e->type = t->type;
    return e;
}

Expr* Sema::checkTernary(TernaryExpr* e) {
    e->cond = checkCondition(e->cond);
    e->thenExpr = check(e->thenExpr);
    e->elseExpr = check(e->elseExpr);
    Expr*& a = e->thenExpr;
    Expr*& b = e->elseExpr;
    if (!a->type || !b->type) return errorExpr(e);
    if (isUntyped(a) && isUntyped(b)) {
        // Stay untyped; the context (assignment, argument, operator) picks the type later.
        if (isUntypedFloat(a) != isUntypedFloat(b)) {
            Expr*& intSide = isUntypedFloat(a) ? b : a;
            intSide = foldCast(intSide, types_.f64(), false);
            if (intSide->kind == ExprKind::FloatLit) intSide->as<FloatLitExpr>()->untyped = true;
        }
        e->type = isUntypedFloat(a) ? types_.f64() : (a->type->size >= b->type->size ? a->type : b->type);
        return e;
    }
    if (a->type == b->type && !isUntyped(a) && !isUntyped(b)) {
        e->type = a->type;
    } else if (isUntyped(a) && !isUntyped(b)) {
        a = coerce(a, b->type, "conditional expression");
        e->type = b->type;
    } else if (isUntyped(b) && !isUntyped(a)) {
        b = coerce(b, a->type, "conditional expression");
        e->type = a->type;
    } else if (conversionCost(a, b->type) >= 0) {
        a = coerce(a, b->type, "conditional expression");
        e->type = b->type;
    } else if (conversionCost(b, a->type) >= 0) {
        b = coerce(b, a->type, "conditional expression");
        e->type = a->type;
    } else {
        diags_.error(e->loc, "branches have different types: '" + typeName(a->type) + "' and '" + typeName(b->type) + "'");
        return errorExpr(e);
    }
    if (isUntyped(a)) {
        a = foldCast(a, a->type, false);
        b = coerce(b, a->type, "conditional expression");
        e->type = a->type;
    }
    return e;
}

Expr* Sema::checkCall(CallExpr* e) {
    for (Expr*& a : e->args) a = check(a);
    for (Expr* a : e->args)
        if (!a->type) return errorExpr(e);

    // Method-call syntax: base.name(args) → name(base, args)
    if (e->callee->kind == ExprKind::Member) {
        auto* m = e->callee->as<MemberExpr>();
        m->base = check(m->base);
        if (!m->base->type) return errorExpr(e);
        Type* st = m->base->type->isPointer() ? m->base->type->elem : m->base->type;
        if (st->isStruct()) {
            for (const FieldDecl& f : st->structDecl->fields)
                if (f.name == m->name) {
                    diags_.error(e->loc, "'" + m->name + "' is a field, not a method");
                    return errorExpr(e);
                }
        }
        std::vector<Expr*> args;
        args.push_back(m->base);
        args.insert(args.end(), e->args.begin(), e->args.end());
        e->func = resolveOverload(m->name, args, e->loc, true, m->base);
        if (!e->func) return errorExpr(e);
        e->args = std::move(args);
        e->type = e->func->returnType;
        return e;
    }

    if (e->callee->kind != ExprKind::Ident) {
        diags_.error(e->loc, "only named functions can be called");
        return errorExpr(e);
    }
    const std::string& name = e->callee->as<IdentExpr>()->name;
    if (lookupLocal(name)) {
        diags_.error(e->loc, "'" + name + "' is a variable, not a function");
        return errorExpr(e);
    }

    // Built-in intrinsics
    if (name == "sqrt" && !funcs_.count("sqrt") && e->args.size() == 1) {
        Expr*& a = e->args[0];
        Type* t = a->type;
        if (isUntyped(a) || t->isInt()) {
            a = coerce(a, types_.f64(), "argument");
            t = types_.f64();
        }
        if (!t || !t->isFloat()) {
            diags_.error(e->loc, "sqrt needs a float argument");
            return errorExpr(e);
        }
        e->intrinsic = t->size == 4 ? Intrinsic::SqrtF32 : Intrinsic::SqrtF64;
        e->type = t;
        return e;
    }

    std::vector<Type*> explicitArgs;
    for (TypeExpr* t : e->callee->as<IdentExpr>()->typeArgs) {
        Type* r = resolveType(t);
        if (!r) return errorExpr(e);
        explicitArgs.push_back(r);
    }
    e->func = resolveOverload(name, e->args, e->loc, true, nullptr, &explicitArgs);
    if (!e->func) return errorExpr(e);
    e->type = e->func->returnType;
    return e;
}

Expr* Sema::checkIndex(IndexExpr* e) {
    e->base = check(e->base);
    e->index = check(e->index);
    if (!e->base->type || !e->index->type) return errorExpr(e);
    Type* bt = e->base->type;
    if (!bt->isArray() && !(bt->isPointer() && !bt->isVoidPointer())) {
        diags_.error(e->loc, "cannot index a value of type '" + typeName(bt) + "'");
        return errorExpr(e);
    }
    if (!e->index->type->isInt()) {
        diags_.error(e->index->loc, "array index must be an integer");
        return errorExpr(e);
    }
    if (bt->isArray() && e->index->kind == ExprKind::IntLit) {
        auto* lit = e->index->as<IntLitExpr>();
        if (lit->negative || lit->value >= bt->count)
            diags_.error(e->index->loc, "index " + std::to_string(static_cast<int64_t>(lit->value)) +
                                            " is out of bounds for '" + typeName(bt) + "'");
    }
    e->index = toOffset(e->index);
    e->type = bt->elem;
    e->isLvalue = true;
    return e;
}

Expr* Sema::checkMember(MemberExpr* e) {
    e->base = check(e->base);
    Type* bt = e->base->type;
    if (!bt) return errorExpr(e);
    if (bt->isArray() && (e->name == "len" || e->name == "count")) {
        auto* lit = arena_.make<IntLitExpr>(e->loc);
        lit->value = bt->count;
        setUntypedIntType(lit);
        return lit;
    }
    Type* st = bt;
    if (bt->isPointer()) {
        st = bt->elem;
        e->throughPointer = true;
    }
    if (!st->isStruct()) {
        diags_.error(e->loc, "type '" + typeName(bt) + "' has no field '" + e->name + "'");
        return errorExpr(e);
    }
    for (const FieldDecl& f : st->structDecl->fields) {
        if (f.name == e->name) {
            if (!f.type) return errorExpr(e);
            e->offset = f.offset;
            e->type = f.type;
            e->isLvalue = e->throughPointer || e->base->isLvalue;
            return e;
        }
    }
    diags_.error(e->loc, "struct '" + st->name + "' has no field '" + e->name + "'");
    return errorExpr(e);
}

Expr* Sema::checkCast(CastExpr* e) {
    e->operand = check(e->operand);
    Type* to = resolveType(e->target);
    if (!e->operand->type || !to) return errorExpr(e);
    Type* from = e->operand->type;
    if (!castAllowed(from, to)) {
        diags_.error(e->loc, "cannot cast '" + typeName(from) + "' to '" + typeName(to) + "'");
        return errorExpr(e);
    }
    if (e->operand->kind == ExprKind::IntLit || e->operand->kind == ExprKind::FloatLit ||
        e->operand->kind == ExprKind::NullLit) {
        Expr* folded = foldCast(e->operand, to, true);
        if (folded->kind != ExprKind::Cast) return folded;
        e->operand = folded->as<CastExpr>()->operand;
    }
    if (from == to) return e->operand;
    e->type = to;
    return e;
}

Expr* Sema::checkStructLit(StructLitExpr* e) {
    auto it = structs_.find(e->typeName);
    if (it == structs_.end()) {
        diags_.error(e->loc, "unknown struct '" + e->typeName + "'");
        return errorExpr(e);
    }
    StructDecl* s = it->second;
    if (!s->typeParams.empty()) {
        if (e->typeArgs.size() != s->typeParams.size()) {
            diags_.error(e->loc, "generic struct '" + s->name + "' needs type arguments: " + s->name + "::<...>{...}");
            return errorExpr(e);
        }
        std::vector<Type*> args;
        for (TypeExpr* t : e->typeArgs) {
            Type* r = resolveType(t);
            if (!r) return errorExpr(e);
            args.push_back(r);
        }
        s = instantiateStruct(s, args)->structDecl;
    } else if (!e->typeArgs.empty()) {
        diags_.error(e->loc, "struct '" + s->name + "' is not generic");
        return errorExpr(e);
    }

    // Named initializers: Vec3{x: 1.0, z: 2.0}; omitted fields are zero.
    bool named = false;
    for (const std::string& n : e->fieldNames) named |= !n.empty();
    if (named) {
        std::vector<Expr*> full(s->fields.size(), nullptr);
        for (size_t i = 0; i < e->fields.size(); ++i) {
            const std::string& n = e->fieldNames[i];
            if (n.empty()) {
                diags_.error(e->fields[i]->loc, "cannot mix positional and named fields in a struct literal");
                return errorExpr(e);
            }
            size_t idx = 0;
            while (idx < s->fields.size() && s->fields[idx].name != n) ++idx;
            if (idx == s->fields.size()) {
                diags_.error(e->fields[i]->loc, "struct '" + s->name + "' has no field '" + n + "'");
                return errorExpr(e);
            }
            if (full[idx]) {
                diags_.error(e->fields[i]->loc, "field '" + n + "' is given twice");
                return errorExpr(e);
            }
            Expr* v = check(e->fields[i]);
            if (!v->type || !s->fields[idx].type) return errorExpr(e);
            full[idx] = coerce(v, s->fields[idx].type, ("field '" + n + "'").c_str());
            if (!full[idx]->type) return errorExpr(e);
        }
        e->fields = std::move(full);
        e->fieldNames.clear();
        e->type = s->type;
        return e;
    }
    e->fieldNames.clear();
    for (Expr*& f : e->fields) f = check(f);
    if (!e->fields.empty() && e->fields.size() != s->fields.size()) {
        diags_.error(e->loc, "struct '" + s->name + "' has " + std::to_string(s->fields.size()) + " fields, but " +
                                 std::to_string(e->fields.size()) + " values were given");
        return errorExpr(e);
    }
    for (size_t i = 0; i < e->fields.size(); ++i) {
        if (!e->fields[i]->type || !s->fields[i].type) return errorExpr(e);
        e->fields[i] = coerce(e->fields[i], s->fields[i].type, ("field '" + s->fields[i].name + "'").c_str());
        if (!e->fields[i]->type) return errorExpr(e);
    }
    e->type = s->type;
    return e;
}

} // namespace ron
