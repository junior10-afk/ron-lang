#include "ast.hpp"

namespace ron {

const char* binaryOpText(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Mod: return "%";
    case BinaryOp::BitAnd: return "&";
    case BinaryOp::BitOr: return "|";
    case BinaryOp::BitXor: return "^";
    case BinaryOp::Shl: return "<<";
    case BinaryOp::Shr: return ">>";
    case BinaryOp::Eq: return "==";
    case BinaryOp::Ne: return "!=";
    case BinaryOp::Lt: return "<";
    case BinaryOp::Le: return "<=";
    case BinaryOp::Gt: return ">";
    case BinaryOp::Ge: return ">=";
    case BinaryOp::LogAnd: return "&&";
    case BinaryOp::LogOr: return "||";
    }
    return "?";
}

const char* unaryOpText(UnaryOp op) {
    switch (op) {
    case UnaryOp::Neg: return "-";
    case UnaryOp::Not: return "!";
    case UnaryOp::BitNot: return "~";
    case UnaryOp::AddrOf: return "&";
    case UnaryOp::Deref: return "*";
    case UnaryOp::PreInc:
    case UnaryOp::PostInc: return "++";
    case UnaryOp::PreDec:
    case UnaryOp::PostDec: return "--";
    }
    return "?";
}

// ───────────────────────────── Cloning ─────────────────────────────
// Copies only syntactic fields: clones are made from templates that sema never
// annotated. Type expressions are immutable and shared.

Expr* cloneExpr(AstArena& a, Expr* e) {
    if (!e) return nullptr;
    switch (e->kind) {
    case ExprKind::IntLit: {
        auto* src = e->as<IntLitExpr>();
        auto* c = a.make<IntLitExpr>(e->loc);
        c->value = src->value;
        c->negative = src->negative;
        c->untyped = src->untyped;
        return c;
    }
    case ExprKind::FloatLit: {
        auto* c = a.make<FloatLitExpr>(e->loc);
        c->value = e->as<FloatLitExpr>()->value;
        c->untyped = e->as<FloatLitExpr>()->untyped;
        return c;
    }
    case ExprKind::StringLit: {
        auto* c = a.make<StringLitExpr>(e->loc);
        c->value = e->as<StringLitExpr>()->value;
        return c;
    }
    case ExprKind::BoolLit: {
        auto* c = a.make<BoolLitExpr>(e->loc);
        c->value = e->as<BoolLitExpr>()->value;
        return c;
    }
    case ExprKind::NullLit:
        return a.make<NullLitExpr>(e->loc);
    case ExprKind::Ident: {
        auto* c = a.make<IdentExpr>(e->loc);
        c->name = e->as<IdentExpr>()->name;
        c->typeArgs = e->as<IdentExpr>()->typeArgs;
        return c;
    }
    case ExprKind::Path: {
        auto* c = a.make<PathExpr>(e->loc);
        c->scope = e->as<PathExpr>()->scope;
        c->member = e->as<PathExpr>()->member;
        c->typeArgs = e->as<PathExpr>()->typeArgs;
        return c;
    }
    case ExprKind::Unary: {
        auto* c = a.make<UnaryExpr>(e->loc);
        c->op = e->as<UnaryExpr>()->op;
        c->operand = cloneExpr(a, e->as<UnaryExpr>()->operand);
        return c;
    }
    case ExprKind::Binary: {
        auto* src = e->as<BinaryExpr>();
        auto* c = a.make<BinaryExpr>(e->loc);
        c->op = src->op;
        c->lhs = cloneExpr(a, src->lhs);
        c->rhs = cloneExpr(a, src->rhs);
        return c;
    }
    case ExprKind::Assign: {
        auto* src = e->as<AssignExpr>();
        auto* c = a.make<AssignExpr>(e->loc);
        c->compound = src->compound;
        c->op = src->op;
        c->target = cloneExpr(a, src->target);
        c->value = cloneExpr(a, src->value);
        return c;
    }
    case ExprKind::Ternary: {
        auto* src = e->as<TernaryExpr>();
        auto* c = a.make<TernaryExpr>(e->loc);
        c->cond = cloneExpr(a, src->cond);
        c->thenExpr = cloneExpr(a, src->thenExpr);
        c->elseExpr = cloneExpr(a, src->elseExpr);
        return c;
    }
    case ExprKind::Call: {
        auto* src = e->as<CallExpr>();
        auto* c = a.make<CallExpr>(e->loc);
        c->callee = cloneExpr(a, src->callee);
        for (Expr* arg : src->args) c->args.push_back(cloneExpr(a, arg));
        return c;
    }
    case ExprKind::Index: {
        auto* c = a.make<IndexExpr>(e->loc);
        c->base = cloneExpr(a, e->as<IndexExpr>()->base);
        c->index = cloneExpr(a, e->as<IndexExpr>()->index);
        return c;
    }
    case ExprKind::Member: {
        auto* c = a.make<MemberExpr>(e->loc);
        c->base = cloneExpr(a, e->as<MemberExpr>()->base);
        c->name = e->as<MemberExpr>()->name;
        return c;
    }
    case ExprKind::Cast: {
        auto* c = a.make<CastExpr>(e->loc);
        c->operand = cloneExpr(a, e->as<CastExpr>()->operand);
        c->target = e->as<CastExpr>()->target;
        return c;
    }
    case ExprKind::Sizeof: {
        auto* c = a.make<SizeofExpr>(e->loc);
        c->target = e->as<SizeofExpr>()->target;
        return c;
    }
    case ExprKind::StructLit: {
        auto* src = e->as<StructLitExpr>();
        auto* c = a.make<StructLitExpr>(e->loc);
        c->typeName = src->typeName;
        c->typeArgs = src->typeArgs;
        c->fieldNames = src->fieldNames;
        for (Expr* f : src->fields) c->fields.push_back(cloneExpr(a, f));
        return c;
    }
    case ExprKind::ArrayLit: {
        auto* c = a.make<ArrayLitExpr>(e->loc);
        for (Expr* x : e->as<ArrayLitExpr>()->elems) c->elems.push_back(cloneExpr(a, x));
        return c;
    }
    case ExprKind::Try: {
        auto* c = a.make<TryExpr>(e->loc);
        c->operand = cloneExpr(a, e->as<TryExpr>()->operand);
        return c;
    }
    case ExprKind::Variant:
        return e;   // only created by sema, never part of an unchecked template
    case ExprKind::Slice: {
        auto* src = e->as<SliceExpr>();
        auto* c = a.make<SliceExpr>(e->loc);
        c->base = cloneExpr(a, src->base);
        c->lo = cloneExpr(a, src->lo);
        c->hi = cloneExpr(a, src->hi);
        return c;
    }
    }
    return nullptr;
}

Stmt* cloneStmt(AstArena& a, Stmt* s) {
    if (!s) return nullptr;
    switch (s->kind) {
    case StmtKind::Block: {
        auto* c = a.make<BlockStmt>(s->loc);
        for (Stmt* st : s->as<BlockStmt>()->stmts) c->stmts.push_back(cloneStmt(a, st));
        return c;
    }
    case StmtKind::VarDecl: {
        auto* src = s->as<VarDeclStmt>();
        auto* c = a.make<VarDeclStmt>(s->loc);
        c->typeExpr = src->typeExpr;
        c->name = src->name;
        c->init = cloneExpr(a, src->init);
        c->isConst = src->isConst;
        return c;
    }
    case StmtKind::Expr: {
        auto* c = a.make<ExprStmt>(s->loc);
        c->expr = cloneExpr(a, s->as<ExprStmt>()->expr);
        return c;
    }
    case StmtKind::If: {
        auto* src = s->as<IfStmt>();
        auto* c = a.make<IfStmt>(s->loc);
        c->cond = cloneExpr(a, src->cond);
        c->thenStmt = cloneStmt(a, src->thenStmt);
        c->elseStmt = cloneStmt(a, src->elseStmt);
        return c;
    }
    case StmtKind::While: {
        auto* c = a.make<WhileStmt>(s->loc);
        c->cond = cloneExpr(a, s->as<WhileStmt>()->cond);
        c->body = cloneStmt(a, s->as<WhileStmt>()->body);
        c->label = s->as<WhileStmt>()->label;
        return c;
    }
    case StmtKind::DoWhile: {
        auto* c = a.make<DoWhileStmt>(s->loc);
        c->body = cloneStmt(a, s->as<DoWhileStmt>()->body);
        c->cond = cloneExpr(a, s->as<DoWhileStmt>()->cond);
        c->label = s->as<DoWhileStmt>()->label;
        return c;
    }
    case StmtKind::Switch: {
        auto* src = s->as<SwitchStmt>();
        auto* c = a.make<SwitchStmt>(s->loc);
        c->subject = cloneExpr(a, src->subject);
        for (const SwitchCase& sc : src->cases) {
            SwitchCase copy;
            copy.loc = sc.loc;
            copy.isDefault = sc.isDefault;
            for (Expr* v : sc.values) copy.values.push_back(cloneExpr(a, v));
            for (Stmt* st : sc.body) copy.body.push_back(cloneStmt(a, st));
            c->cases.push_back(std::move(copy));
        }
        return c;
    }
    case StmtKind::Match: {
        auto* src = s->as<MatchStmt>();
        auto* c = a.make<MatchStmt>(s->loc);
        c->subject = cloneExpr(a, src->subject);
        for (const MatchArm& arm : src->arms) {
            MatchArm copy;
            copy.variant = arm.variant;
            copy.bindings = arm.bindings;
            copy.hasParens = arm.hasParens;
            copy.loc = arm.loc;
            copy.body = cloneStmt(a, arm.body);
            c->arms.push_back(std::move(copy));
        }
        return c;
    }
    case StmtKind::ForIn: {
        auto* src = s->as<ForInStmt>();
        auto* c = a.make<ForInStmt>(s->loc);
        c->varType = src->varType;
        c->name = src->name;
        c->iterable = cloneExpr(a, src->iterable);
        c->rangeEnd = cloneExpr(a, src->rangeEnd);
        c->body = cloneStmt(a, src->body);
        c->label = src->label;
        return c;
    }
    case StmtKind::Defer: {
        auto* c = a.make<DeferStmt>(s->loc);
        c->body = cloneStmt(a, s->as<DeferStmt>()->body);
        return c;
    }
    case StmtKind::For: {
        auto* src = s->as<ForStmt>();
        auto* c = a.make<ForStmt>(s->loc);
        c->init = cloneStmt(a, src->init);
        c->cond = cloneExpr(a, src->cond);
        c->step = cloneExpr(a, src->step);
        c->body = cloneStmt(a, src->body);
        c->label = src->label;
        return c;
    }
    case StmtKind::Return: {
        auto* c = a.make<ReturnStmt>(s->loc);
        c->value = cloneExpr(a, s->as<ReturnStmt>()->value);
        return c;
    }
    case StmtKind::Break: {
        auto* c = a.make<BreakStmt>(s->loc);
        c->label = s->as<BreakStmt>()->label;
        return c;
    }
    case StmtKind::Continue: {
        auto* c = a.make<ContinueStmt>(s->loc);
        c->label = s->as<ContinueStmt>()->label;
        return c;
    }
    }
    return nullptr;
}

} // namespace ron
