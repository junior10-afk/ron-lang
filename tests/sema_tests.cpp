#include "test.hpp"

#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"

using namespace ron;

namespace {

SourceManager& semaSources() {
    static SourceManager s;
    return s;
}

// Returns all diagnostics joined; empty string means the program is valid.
std::string semaErrors(const std::string& src) {
    uint32_t id = semaSources().add("test.ron", src);
    Diagnostics diags(semaSources());
    AstArena arena;
    Program program;
    Lexer lexer(semaSources().get(id), id, diags);
    Parser parser(lexer.tokenizeAll(), arena, diags);
    parser.parseProgram(program);
    if (!diags.hasErrors()) {
        TypeContext types;
        Sema sema(program, arena, types, diags);
        sema.run();
    }
    std::string out;
    for (const auto& d : diags.all())
        if (d.severity == Severity::Error) out += d.message + "\n";
    return out;
}

bool valid(const std::string& body) {
    std::string e = semaErrors(body);
    if (!e.empty()) std::printf("    unexpected errors:\n%s", e.c_str());
    return e.empty();
}

bool fails(const std::string& body, const std::string& fragment) {
    std::string e = semaErrors(body);
    bool ok = e.find(fragment) != std::string::npos;
    if (!ok) std::printf("    expected error containing '%s', got:\n%s", fragment.c_str(), e.empty() ? "(none)\n" : e.c_str());
    return ok;
}

} // namespace

TEST(sema_valid_program) {
    CHECK(valid(R"(
        struct Vec3 { float x, y, z; }
        fn +(Vec3 a, Vec3 b) -> Vec3 { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
        fn len(Vec3 v) -> float { return sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
        enum Color { Red, Green = 5, Blue }
        const int MAX = 10 * 2;
        int counter = MAX;
        fn main() -> int {
            Vec3 a = Vec3{1.0, 2.0, 3.0};
            Vec3 b = a + a;
            float l = b.len();
            Color c = Color::Blue;
            int[MAX] arr;
            for (int i = 0; i < arr.len; i++) { arr[i] = i * 2; }
            Vec3* p = &a;
            p.x = 5.0;
            double d = l;
            u8 small = 255;
            i64 big = small;
            float f = 3;
            f *= 0.5;
            return counter + (c as int);
        }
    )"));
}

TEST(sema_literal_ranges) {
    CHECK(valid("fn main() -> int { u8 a = 255; i8 b = -128; return 0; }"));
    CHECK(fails("fn main() -> int { u8 a = 256; return 0; }", "does not fit"));
    CHECK(fails("fn main() -> int { u32 a = -1; return 0; }", "does not fit"));
    CHECK(fails("fn main() -> int { int a = 1.5; return 0; }", "float literal"));
}

TEST(sema_type_errors) {
    CHECK(fails("fn main() -> int { int x = true; return 0; }", "cannot convert 'bool' to 'i32'"));
    CHECK(fails("fn main() -> int { float f = 1.0; int i = f; return 0; }", "explicit cast"));
    CHECK(fails("fn main() -> int { i64 a = 1; i32 b = a; return 0; }", "cannot convert"));
    CHECK(fails("fn main() -> int { int x = 1; if (x) { } return 0; }", "condition must be a bool"));
    CHECK(fails("fn main() -> int { return y; }", "unknown name 'y'"));
    CHECK(fails("fn main() -> int { Foo f; return 0; }", "unknown type 'Foo'"));
}

TEST(sema_functions_and_overloads) {
    CHECK(valid(R"(
        fn show(i64 v) { }
        fn show(f64 v) { }
        fn show(u8* s) { }
        fn main() -> int { show(1); show(1.5); show("hi"); int x = 3; show(x); return 0; }
    )"));
    CHECK(fails("fn f(int a) {} fn main() -> int { f(1, 2); return 0; }", "no matching function"));
    CHECK(fails("fn f(int a) {} fn f(int b) {} fn main() -> int { return 0; }", "already defined"));
    CHECK(fails("fn f() -> int { } fn main() -> int { return 0; }", "does not return a value"));
    CHECK(fails("fn main() -> int { break; return 0; }", "outside of a loop"));
}

TEST(sema_structs) {
    CHECK(fails("struct A { A inner; } fn main() -> int { return 0; }", "contains itself"));
    CHECK(valid("struct Node { int v; Node* next; } fn main() -> int { Node n; n.next = &n; return n.next.v; }"));
    CHECK(fails("struct P { int x; } fn main() -> int { P p; return p.y; }", "no field 'y'"));
    CHECK(fails("struct P { int x, y; } fn main() -> int { P p = P{1}; return 0; }", "has 2 fields"));
    CHECK(fails("struct P { int x; } fn main() -> int { P a; P b; P c = a + b; return 0; }", "no operator '+'"));
}

TEST(sema_constness_and_lvalues) {
    CHECK(fails("fn main() -> int { const int x = 1; x = 2; return 0; }", "cannot assign to const"));
    CHECK(fails("fn main() -> int { 5 = 3; return 0; }", "not assignable"));
    CHECK(fails("fn main() -> int { int* p = &5; return 0; }", "address of a temporary"));
}

TEST(sema_generics) {
    CHECK(valid(R"(
        struct Box<T> { T value; }
        fn get<T>(Box<T>* b) -> T { return b.value; }
        fn main() -> int { Box<int> b; b.value = 3; Box<Box<int>> nested; return b.get() + nested.value.value; }
    )"));
    CHECK(fails("struct L<T> { T v; } fn main() -> int { L x; return 0; }", "needs 1 type argument"));
    CHECK(fails("struct P { int x; } fn main() -> int { P<int> p; return 0; }", "is not generic"));
    CHECK(fails("fn id<T>(T v) -> T { return v.missing; } fn main() -> int { return id(1); }", "has no field"));
    CHECK(fails("fn make<T>() -> T { T v; return v; } fn main() -> int { return make(); }", "no matching function"));
}

TEST(sema_requires_main) {
    CHECK(fails("fn notmain() {}", "no 'main' function"));
}
