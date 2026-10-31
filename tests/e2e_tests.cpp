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

TEST(e2e_register_pressure) {
    CHECK(exitsWith("regs", R"(
        fn touch(int* p) { *p += 1; }
        fn mix(float a, float b) -> float { return a * 2.0 + b; }
        fn main() -> int {
            int a = 1; int b = 2; int c = 3; int d = 4; int e = 5;
            int f = 6; int g = 7; int h = 8; int i = 9; int j = 10;   // more ints than registers
            float x0 = 0.5; float x1 = 1.0; float x2 = 1.5; float x3 = 2.0; float x4 = 2.5; float x5 = 3.0;
            float x6 = 3.5; float x7 = 4.0; float x8 = 4.5; float x9 = 5.0; float x10 = 5.5; float x11 = 6.0;
            int addr = 100;
            touch(&addr);                          // address taken: must stay in memory
            for (int k = 0; k < 3; k++) {
                a += b; b += c; c += d; d += e; e += f; f += g; g += h; h += i; i += j; j += a;
                x0 = mix(x0, x11);                 // calls must not clobber register variables
                x11 += x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8 + x9 + x10;
            }
            u8 small = 250;
            small += 10;                           // narrow register variable wraps to 4
            int fsum = (x0 + x11) as int;
            return (a + b + c + d + e + f + g + h + i + j) % 100 + addr + (small as int) + fsum % 7;
        }
    )", 181));
}

TEST(e2e_generics) {
    CHECK(prints("generics", R"(
        import "std/io.ron";
        struct Pair<A, B> { A first; B second; }
        struct Node<T> { T value; Node<T>* next; }       // self-reference through a pointer
        fn make_pair<A, B>(A a, B b) -> Pair<A, B> { return Pair::<A, B>{a, b}; }
        fn swap<T>(T* a, T* b) { T t = *a; *a = *b; *b = t; }
        fn max_of<T>(T a, T b) -> T { return a > b ? a : b; }
        fn zero<T>() -> T { T z; return z; }
        fn sum<T>(Node<T>* n) -> T {
            T total = zero::<T>();
            while (n != null) { total += n.value; n = n.next; }
            return total;
        }
        fn main() -> int {
            Pair<int, float> p = make_pair(3, 2.5f);
            println(p.first);
            println(p.second);
            int a = 1;
            int b = 2;
            swap(&a, &b);
            println(a * 10 + b);
            println(max_of(4, 9));
            println(max_of(1.5, -2.0));
            Node<i64> n3 = Node::<i64>{30, null};
            Node<i64> n2 = Node::<i64>{20, &n3};
            Node<i64> n1 = Node::<i64>{10, &n2};
            println(sum(&n1));
            Node<float> f2 = Node::<float>{0.25, null};
            Node<float> f1 = Node::<float>{0.5, &f2};
            println(sum(&f1));
            println(sizeof(Pair<u8, i64>) as i64);
            return 0;
        }
    )", "3\n2.5\n21\n9\n1.5\n60\n0.75\n16\n"));
}

TEST(e2e_list_map_string) {
    CHECK(prints("containers", R"(
        import "std/io.ron";
        import "std/list.ron";
        import "std/map.ron";
        import "std/string.ron";
        struct Enemy { int hp; float x; }
        fn <(Enemy a, Enemy b) -> bool { return a.hp < b.hp; }
        fn main() -> int {
            List<int> nums;
            for (int i = 0; i < 100; i++) nums.push(i * i);
            println(nums.len);
            println(nums.get(99));
            nums.remove_swap(0);
            println(nums.get(0));
            println(nums.contains(81));

            List<Enemy> enemies;
            enemies.push(Enemy{30, 1.0});
            enemies.push(Enemy{10, 2.0});
            enemies.push(Enemy{20, 3.0});
            enemies.sort();
            enemies.at(0).hp += 5;
            println(enemies.get(0).hp * 100 + enemies.get(1).hp + enemies.get(2).hp);

            Map<int, float> scores;
            for (int i = 0; i < 1000; i++) scores.put(i, (i as float) * 0.5);
            println(scores.count);
            println(*scores.get(500));
            println(scores.has(1000));
            scores.remove(500);
            println(scores.has(500) || scores.get(501) == null);

            Map<String, int> ids;
            ids.put(string("player"), 1);
            ids.put(string("enemy"), 2);
            println(*ids.get(string("enemy")));

            String s = string("Score: ");
            s.append(42);
            s.append(" / ");
            s.append(3.25);
            println(s);
            String t = s + "!";
            println(t.len);
            println(t.find("42"));
            println(t.substring(0, 5).to_upper());
            println(parse_int(string("-1234")) + 1);
            println(t == s);
            s.free_string();
            t.free_string();
            nums.free_list();
            return 0;
        }
    )", "100\n9801\n9801\ntrue\n1550\n1000\n250.0\nfalse\nfalse\n2\nScore: 42 / 3.25\n17\n7\nSCORE\n-1233\nfalse\n"));
}

TEST(e2e_syntax_v01) {
    CHECK(prints("syntax01", R"(
        import "std/io.ron";
        enum Dir { North, East, South, West }
        fn name(Dir d) -> u8* {
            switch (d) {
            case Dir::North: return "N";
            case Dir::East: return "E";
            case Dir::South, Dir::West: return "SW";
            }
        }
        struct V { float x, y, z; }
        var counter = 10;
        fn work(int n) -> int {
            defer println("leave work");
            for (var i = 0; i < 3; i++) {
                defer print("d");
                if (i == n) return i * 100;
            }
            println("");
            return -1;
        }
        fn main() -> int {
            var a = 5;
            var f = 2.5;
            const k = 3;
            println(a + k);
            println(f * 2.0);
            println(name(Dir::West));
            switch (a) {
            case 1, 2: println("small");
            case 5:
                println("five");
                break;
            default: println("other");
            }
            var arr = [1, 2, 3, 4];
            int[6] b = [7, 8];
            println(arr[3] + b[1] + b[5] + arr.len);
            V v = V{y: 2.0};
            println(v.x + v.y + v.z);
            println(work(1));
            println(work(9));
            int found = -1;
            outer: for (var i = 0; i < 5; i++) {
                for (var j = 0; j < 5; j++) {
                    if (i * j == 6) {
                        found = i * 10 + j;
                        break outer;
                    }
                    if (j > i) continue outer;
                }
            }
            println(found);
            var n = 0;
            do { n += 3; } while (n < 10);
            println(n);
            counter += 1;
            println(counter);
            return 0;
        }
    )", "8\n5.0\nSW\nfive\n16\n2.0\nddleave work\n100\nddd\nleave work\n-1\n23\n12\n11\n"));
}

TEST(e2e_std_math) {
    CHECK(prints("math", R"(
        import "std/io.ron";
        import "std/math.ron";
        fn show(float v) { println((v * 1000.0 + (v < 0.0 ? -0.5 : 0.5)) as int); }   // 3 decimals, rounded
        fn main() -> int {
            show(sin(PI / 6.0));
            show(cos(0.0));
            show(cos(PI));
            show(sin(-7.0 * PI / 2.0));
            show(atan2(1.0, 1.0));
            show(floor(-1.5));
            show(fmod(7.5, 2.0));
            Vec3 c = cross(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0));
            show(c.z);
            show(length(vec3(3.0, 4.0, 0.0)));
            Mat4 m = mat4_translate(vec3(1.0, 2.0, 3.0)) * mat4_scale(vec3(2.0, 2.0, 2.0));
            Vec3 p = transform_point(m, vec3(1.0, 1.0, 1.0));
            show(p.x + p.y + p.z);   // 3 + 4 + 5
            return 0;
        }
    )", "500\n1000\n-1000\n1000\n785\n-2000\n1500\n1000\n5000\n12000\n"));
}

TEST(e2e_std_physics_and_ecs) {
    // Uses the ECS (which references the engine) without calling any engine
    // function: dead-code elimination keeps ron_engine.dll out of the imports.
    CHECK(prints("physics", R"(
        import "std/io.ron";
        import "std/ecs.ron";
        fn main() -> int {
            PhysicsWorld p = physics_world(16);
            int ball = p.add_sphere(vec3(0.0, 10.0, 0.0), 0.5, 1.0);
            p.body(ball).restitution = 0.0;
            int crate = p.add_box(vec3(3.0, 5.0, 0.0), vec3(0.5, 0.5, 0.5), 1.0);
            int floor_box = p.add_box(vec3(3.0, 0.5, 0.0), vec3(2.0, 0.5, 2.0), 0.0);
            for (int i = 0; i < 240; i++) p.step(1.0 / 60.0);
            println((p.body(ball).position.y * 100.0 + 0.5) as int);    // rests on the ground: 50
            println((p.body(crate).position.y * 100.0 + 0.5) as int);   // rests on the static box: 150
            println(p.body(ball).grounded);

            World w = world_create(8);
            int a = w.spawn();
            int b = w.spawn();
            w.set_velocity(b, vec3(1.0, 0.0, 0.0));
            w.set_lifetime(a, 0.5);
            for (int i = 0; i < 10; i++) {
                system_movement(&w, 0.1);
                system_lifetime(&w, &p, 0.1);
            }
            println(w.alive);                                            // a expired: 1
            println((w.transforms[b].position.x * 10.0 + 0.5) as int);   // 10
            return 0;
        }
    )", "50\n150\ntrue\n1\n10\n"));
}

TEST(e2e_std_strings_and_random) {
    CHECK(prints("strings", R"(
        import "std/io.ron";
        import "std/string.ron";
        import "std/random.ron";
        fn main() -> int {
            StrBuf sb;
            sb.clear();
            sb.append("score=");
            sb.append(-42);
            sb.append("!");
            println(sb.text());
            println(strlen(sb.text()) as i64);
            println(streq("abc", "abc") && !streq("abc", "abd"));
            seed_random(123);
            bool ok = true;
            for (int i = 0; i < 1000; i++) {
                int r = random_int(3, 7);
                float f = random();
                if (r < 3 || r > 7 || f < 0.0 || f >= 1.0) ok = false;
            }
            println(ok);
            return 0;
        }
    )", "score=-42!\n10\ntrue\ntrue\n"));
}

TEST(e2e_compile_errors_are_reported) {
    RunResult r = runProgram("bad", "fn main() -> int { int x = \"text\"; return y; }");
    CHECK(!r.compiled);
    CHECK(r.errors.find("cannot convert 'u8*' to 'i32'") != std::string::npos);
    CHECK(r.errors.find("unknown name 'y'") != std::string::npos);
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
