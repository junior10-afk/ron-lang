// End-to-end tests: compile Ron programs to real .exe files, run them and check
// their exit code and console output.

#include "test.hpp"

#include "driver.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace ron;

namespace {

struct RunResult {
    bool compiled = false;
    int exitCode = -1;
    std::string output;
    std::string errors;
};

std::string runCapture(const std::string& exe, int& exitCode) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe, writePipe;
    CreatePipe(&readPipe, &writePipe, &sa, 0);
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + exe + "\"";
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        exitCode = -1;
        return "<failed to start>";
    }
    CloseHandle(writePipe);
    std::string out;
    char buf[4096];
    DWORD n;
    while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(readPipe);
    if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 999);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    exitCode = static_cast<int>(code);
    return out;
}

RunResult runProgram(const std::string& name, const std::string& source) {
    fs::path dir = fs::temp_directory_path() / "ron_e2e";
    fs::create_directories(dir);
    fs::path src = dir / (name + ".ron");
    fs::path exe = dir / (name + ".exe");
    std::ofstream(src, std::ios::binary) << source;

    RunResult r;
    SourceManager sources;
    Diagnostics diags(sources);
    CompileOptions opts;
    opts.input = src.string();
    opts.output = exe.string();
    opts.stdRoot = RON_SOURCE_DIR;
    r.compiled = compile(opts, sources, diags).ok;
    for (const auto& d : diags.all()) r.errors += diags.format(d);
    if (!r.compiled) {
        std::printf("    compile errors:\n%s", r.errors.c_str());
        return r;
    }
    r.output = runCapture(exe.string(), r.exitCode);
    return r;
}

bool exitsWith(const std::string& name, const std::string& src, int expected) {
    RunResult r = runProgram(name, src);
    if (r.compiled && r.exitCode != expected)
        std::printf("    %s: exit code %d, expected %d\n", name.c_str(), r.exitCode, expected);
    return r.compiled && r.exitCode == expected;
}

bool prints(const std::string& name, const std::string& src, const std::string& expected) {
    RunResult r = runProgram(name, src);
    if (r.compiled && r.output != expected)
        std::printf("    %s: printed\n'%s'\n    expected\n'%s'\n", name.c_str(), r.output.c_str(), expected.c_str());
    return r.compiled && r.output == expected;
}

} // namespace

TEST(e2e_return_code) {
    CHECK(exitsWith("ret42", "fn main() -> int { return 42; }", 42));
    CHECK(exitsWith("ret_void", "fn main() { }", 0));
}

TEST(e2e_arithmetic) {
    CHECK(exitsWith("arith", R"(
        fn main() -> int {
            int a = 7;
            int b = 3;
            int r = a * b + a / b - a % b;   // 21 + 2 - 1 = 22
            r = r << 2;                      // 88
            r = r | 1;                       // 89
            r -= 9;                          // 80
            int neg = -a;                    // -7
            return r + neg + (a > b ? 1 : 0);   // 74
        }
    )", 74));
    CHECK(exitsWith("unsigned_wrap", R"(
        fn main() -> int {
            u8 x = 250;
            x += 10;           // wraps to 4
            i8 y = 127;
            y += 1;            // wraps to -128
            u32 big = 0xFFFFFFFF;
            big += 2;          // 1
            return (x as int) + (y as int) + 200 + (big as int);   // 4 - 128 + 200 + 1 = 77
        }
    )", 77));
}

TEST(e2e_control_flow) {
    CHECK(exitsWith("loops", R"(
        fn main() -> int {
            int sum = 0;
            for (int i = 0; i < 10; i++) {
                if (i == 3) continue;
                if (i == 8) break;
                sum += i;          // 0+1+2+4+5+6+7 = 25
            }
            int n = 0;
            while (true) { n++; if (n >= 5 && !(n < 5)) break; }
            return sum + n;        // 30
        }
    )", 30));
}

TEST(e2e_functions_and_recursion) {
    CHECK(exitsWith("fib", R"(
        fn fib(int n) -> int { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
        fn many(int a, int b, int c, int d, int e, int f) -> int { return a - b + c - d + e - f * 2; }
        fn main() -> int { return fib(10) + many(10, 1, 2, 3, 4, 5); }   // 55 + 2
    )", 57));
}

TEST(e2e_floats) {
    CHECK(exitsWith("floats", R"(
        fn scale(float x, double y, float z, float w, float v) -> float { return x * (y as float) + z - w + v; }
        fn main() -> int {
            float a = 1.5;
            float b = a * 4.0 + 0.5;          // 6.5
            double d = b;                    // f32 -> f64
            d = d * 2.0;                     // 13.0
            float s = sqrt(16.0f);           // 4.0
            float r = scale(2.0, 3.0, 1.0, 0.5, 0.25);   // 6.75
            int i = 10;
            float mixed = i * 0.5;            // 5.0
            if (b > 6.4 && b < 6.6 && s == 4.0) {
                return (d as int) + (r * 4.0) as int + (mixed as int);   // 13 + 27 + 5
            }
            return 0;
        }
    )", 45));
}

TEST(e2e_structs_and_pointers) {
    CHECK(exitsWith("structs", R"(
        struct Vec3 { float x, y, z; }
        struct Pair { int a, b; }
        fn +(Vec3 a, Vec3 b) -> Vec3 { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
        fn *(Vec3 a, float s) -> Vec3 { return Vec3{a.x * s, a.y * s, a.z * s}; }
        fn dot(Vec3 a, Vec3 b) -> float { return a.x * b.x + a.y * b.y + a.z * b.z; }
        fn swap(Pair* p) { int t = p.a; p.a = p.b; p.b = t; }
        fn make_pair(int a, int b) -> Pair { return Pair{a, b}; }
        fn main() -> int {
            Vec3 v = Vec3{1.0, 2.0, 3.0};
            Vec3 w = v + v * 2.0;              // (3, 6, 9)
            float d = w.dot(v);                // 3 + 12 + 27 = 42
            Pair p = make_pair(1, 2);
            swap(&p);
            p.swap();                          // UFCS takes &p automatically
            p.swap();
            Vec3* pv = &w;
            pv.y = 100.0;
            return (d as int) + p.a * 10 + p.b + (w.y as int);   // 42 + 20 + 1 + 100
        }
    )", 163));
}

TEST(e2e_arrays_and_globals) {
    CHECK(exitsWith("arrays", R"(
        int counter = 5;
        const int N = 8;
        struct Grid { int[16] cells; }
        fn fill(Grid* g) { for (int i = 0; i < 16; i++) g.cells[i] = i; }
        fn sum(int* p, int n) -> int { int s = 0; for (int i = 0; i < n; i++) s += p[i]; return s; }
        fn main() -> int {
            int[N] a;
            for (int i = 0; i < a.len; i++) a[i] = i * i;      // 0 1 4 9 16 25 36 49
            Grid g;
            fill(&g);
            counter += 1;
            int* p = &a[0];
            p = p + 2;
            return sum(&a[0], N) - 140 + g.cells[15] + counter + *p;   // 0 + 15 + 6 + 4
        }
    )", 25));
}

TEST(e2e_big_struct_return_and_copy) {
    CHECK(exitsWith("bigstruct", R"(
        struct Mat4 { float[16] m; }
        fn identity() -> Mat4 {
            Mat4 r;
            r.m[0] = 1.0; r.m[5] = 1.0; r.m[10] = 1.0; r.m[15] = 1.0;
            return r;
        }
        fn trace(Mat4 a) -> float { return a.m[0] + a.m[5] + a.m[10] + a.m[15]; }
        fn main() -> int {
            Mat4 a = identity();
            Mat4 b = a;
            b.m[0] = 10.0;
            return (trace(a) as int) * 100 + (trace(b) as int);   // 400 + 13
        }
    )", 413));
}

TEST(e2e_enums_and_casts) {
    CHECK(exitsWith("enums", R"(
        enum State { Idle, Running = 10, Done }
        fn main() -> int {
            State s = State::Running;
            if (s == State::Running) s = State::Done;
            i64 big = 1 << 40;
            int shifted = (big >> 38) as int;     // 4
            u8 b = 300 as u8;                     // 44
            float f = -2.7;
            int t = f as int;                     // -2
            return (s as int) + shifted + (b as int) + t;   // 11 + 4 + 44 - 2
        }
    )", 57));
}

TEST(e2e_hello_world) {
    CHECK(prints("hello", R"(
        import "std/io.ron";
        fn main() -> int {
            println("Hello from Ron!");
            print(42);
            print(" ");
            print(-17);
            print(" ");
            print(3.25);
            print(" ");
            println(true);
            return 0;
        }
    )", "Hello from Ron!\n42 -17 3.25 true\n"));
}
