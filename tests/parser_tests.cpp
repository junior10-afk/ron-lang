#include "test.hpp"

#include "lexer.hpp"
#include "parser.hpp"

using namespace ron;

namespace {

struct Parsed {
    AstArena arena;
    Program program;
    size_t errors = 0;
};

SourceManager& parserSources() {
    static SourceManager s;
    return s;
}

std::unique_ptr<Parsed> parse(std::string src) {
    auto p = std::make_unique<Parsed>();
    uint32_t id = parserSources().add("test.ron", std::move(src));
    Diagnostics diags(parserSources());
    Lexer lexer(parserSources().get(id), id, diags);
    Parser parser(lexer.tokenizeAll(), p->arena, diags);
    parser.parseProgram(p->program);
    p->errors = diags.errorCount();
    return p;
}

// Renders an expression as a fully parenthesized string to check precedence.
std::string show(Expr* e) {
    switch (e->kind) {
    case ExprKind::IntLit: return std::to_string(e->as<IntLitExpr>()->value);
    case ExprKind::Ident: return e->as<IdentExpr>()->name;
    case ExprKind::Binary: {
        auto* b = e->as<BinaryExpr>();
        return "(" + show(b->lhs) + " " + binaryOpText(b->op) + " " + show(b->rhs) + ")";
    }
    case ExprKind::Unary: {
        auto* u = e->as<UnaryExpr>();
        bool post = u->op == UnaryOp::PostInc || u->op == UnaryOp::PostDec;
        return post ? "(" + show(u->operand) + unaryOpText(u->op) + ")"
                    : "(" + std::string(unaryOpText(u->op)) + show(u->operand) + ")";
    }
    case ExprKind::Assign: {
        auto* a = e->as<AssignExpr>();
        return "(" + show(a->target) + (a->compound ? std::string(binaryOpText(a->op)) + "=" : "=") + show(a->value) + ")";
    }
    case ExprKind::Call: {
        auto* c = e->as<CallExpr>();
        std::string s = show(c->callee) + "(";
        for (size_t i = 0; i < c->args.size(); ++i) s += (i ? ", " : "") + show(c->args[i]);
        return s + ")";
    }
    case ExprKind::Member: return show(e->as<MemberExpr>()->base) + "." + e->as<MemberExpr>()->name;
    case ExprKind::Index: return show(e->as<IndexExpr>()->base) + "[" + show(e->as<IndexExpr>()->index) + "]";
    case ExprKind::Cast: return "(" + show(e->as<CastExpr>()->operand) + " as " + e->as<CastExpr>()->target->name + ")";
    case ExprKind::Ternary: {
        auto* t = e->as<TernaryExpr>();
        return "(" + show(t->cond) + " ? " + show(t->thenExpr) + " : " + show(t->elseExpr) + ")";
    }
    default: return "?";
    }
}

std::string exprOf(const std::string& src) {
    auto p = parse("fn f() { " + src + "; }");
    if (p->errors) return "<error>";
    auto* fn = static_cast<FuncDecl*>(p->program.decls[0]);
    auto* stmt = fn->body->stmts[0];
    if (stmt->kind != StmtKind::Expr) return "<not expr>";
    return show(stmt->as<ExprStmt>()->expr);
}

} // namespace

TEST(parser_precedence) {
    CHECK_EQ(exprOf("a + b * c"), "(a + (b * c))");
    // `a * b;` alone would be a declaration (like in C), so test through an assignment.
    CHECK_EQ(exprOf("x = a * b + c"), "(x=((a * b) + c))");
    CHECK_EQ(exprOf("a - b - c"), "((a - b) - c)");
    CHECK_EQ(exprOf("a || b && c"), "(a || (b && c))");
    CHECK_EQ(exprOf("a == b < c"), "(a == (b < c))");
    CHECK_EQ(exprOf("a & b == c"), "(a & (b == c))");
    CHECK_EQ(exprOf("1 << 2 + 3"), "(1 << (2 + 3))");
}

TEST(parser_unary_postfix_cast) {
    CHECK_EQ(exprOf("-a * b"), "((-a) * b)");
    CHECK_EQ(exprOf("*p.x"), "(*p.x)");
    CHECK_EQ(exprOf("a.b.c(1, 2)[3]"), "a.b.c(1, 2)[3]");
    CHECK_EQ(exprOf("i++"), "(i++)");
    CHECK_EQ(exprOf("x as float * 2"), "((x as float) * 2)");
}

TEST(parser_assignment_is_right_associative) {
    CHECK_EQ(exprOf("a = b = c"), "(a=(b=c))");
    CHECK_EQ(exprOf("a += b * 2"), "(a+=(b * 2))");
    CHECK_EQ(exprOf("x = c ? 1 : 2"), "(x=(c ? 1 : 2))");
}

TEST(parser_declarations) {
    auto p = parse(R"(
        import "std/io.ron";
        struct Vec3 { float x, y, z; }
        enum Key { A = 65, B, C }
        const float PI = 3.14;
        int counter = 0;
        extern "kernel32.dll" { fn Sleep(u32 ms); fn GetTickCount() -> u32; }
        fn +(Vec3 a, Vec3 b) -> Vec3 { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
        fn main() -> int { return 0; }
    )");
    CHECK_EQ(p->errors, 0u);
    CHECK_EQ(p->program.decls.size(), 9u);
    auto* s = static_cast<StructDecl*>(p->program.decls[1]);
    CHECK_EQ(s->fields.size(), 3u);
    CHECK_EQ(s->fields[2].name, "z");
    auto* e = static_cast<EnumDecl*>(p->program.decls[2]);
    CHECK_EQ(e->members.size(), 3u);
    auto* sleep = static_cast<FuncDecl*>(p->program.decls[5]);
    CHECK_EQ(sleep->externDll, "kernel32.dll");
    CHECK(sleep->body == nullptr);
    auto* op = static_cast<FuncDecl*>(p->program.decls[7]);
    CHECK_EQ(op->name, "op+");
}

TEST(parser_statements) {
    auto p = parse(R"(
        fn main() -> int {
            int x = 1;
            Vec3* p = null;
            float[16] m;
            for (int i = 0; i < 10; i += 1) { if (i == 5) break; else continue; }
            while (x < 100) x *= 2;
            return x;
        }
    )");
    CHECK_EQ(p->errors, 0u);
    auto* fn = static_cast<FuncDecl*>(p->program.decls[0]);
    CHECK_EQ(fn->body->stmts.size(), 6u);
    CHECK(fn->body->stmts[1]->kind == StmtKind::VarDecl);
    auto* arr = fn->body->stmts[2]->as<VarDeclStmt>();
    CHECK(arr->typeExpr->kind == TypeExpr::Kind::Array);
    CHECK_EQ(arr->typeExpr->arraySize, 16u);
    CHECK(fn->body->stmts[3]->kind == StmtKind::For);
}

TEST(parser_error_recovery) {
    // Two independent errors are both reported.
    auto p = parse(R"(
        fn a() { int x = ; }
        fn b() { return 1 }
        fn c() { return 2; }
    )");
    CHECK_EQ(p->errors, 2u);
    CHECK_EQ(p->program.decls.size(), 3u);
}
