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

RunResult runProgram(const std::string& name, const std::string& source, bool release = false, bool testMode = false) {
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
    opts.release = release;
    opts.testMode = testMode;
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

TEST(e2e_functions_slices_for_in) {
    CHECK(prints("fnslices", R"(
        import "std/io.ron";
        import "std/list.ron";
        fn twice(int x) -> int { return x * 2; }
        fn add(int a, int b) -> int { return a + b; }
        fn add(float a, float b) -> float { return a + b; }
        fn apply(fn(int) -> int f, int v) -> int { return f(v); }
        fn sum(int[] xs) -> int {
            int s = 0;
            for (var x in xs) s += x;
            return s;
        }
        struct Button { u8* label; fn(int) -> int on_click; }
        fn sort_by(int[] xs, fn(int, int) -> bool less) {
            for (var i in 1..xs.len) {
                int key = xs[i];
                var j = i - 1;
                while (j >= 0 && less(key, xs[j])) {
                    xs[j + 1] = xs[j];
                    j--;
                }
                xs[j + 1] = key;
            }
        }
        fn desc(int a, int b) -> bool { return a > b; }
        fn main() -> int {
            println(apply(twice, 21));
            fn(int, int) -> int op = add;
            println(op(3, 4));
            fn(int) -> int none = null;
            println(none == null);
            Button b = Button{"ok", twice};
            println(b.on_click(5));
            int[5] arr = [5, 1, 4, 2, 3];
            println(sum(arr));
            println(sum(arr[1..3]));
            println(sum([10, 20, 30]));
            sort_by(arr, desc);
            for (var x in arr) print(x);
            println();
            for (var p in &arr) *p = *p * 10;
            println(arr[0]);
            List<int> l;
            for (var i in 0..5) l.push(i * i);
            int total = 0;
            for (var v in l) total += v;
            println(total);
            int[] s = l.slice();
            println(s.len);
            println(s[4]);
            var fns = [twice, twice];
            println(fns[1](4));
            return 0;
        }
    )", "42\n7\ntrue\n10\n15\n5\n60\n54321\n50\n30\n5\n16\n8\n"));
}

TEST(e2e_sum_types_match_try) {
    CHECK(prints("sumtypes", R"(
        import "std/io.ron";
        import "std/option.ron";
        enum Shape { Circle(float), Rect(float, float), Empty }
        fn area(Shape s) -> float {
            match (s) {
                Circle(r) => return 3.0 * r * r;
                Rect(w, h) => return w * h;
                Empty => return 0.0;
            }
        }
        enum Tree { Leaf(int), Node(Tree*, Tree*) }
        fn sum(Tree* t) -> int {
            match (*t) {
                Leaf(v) => return v;
                Node(l, r) => return sum(l) + sum(r);
            }
        }
        fn parse_digit(u8 c) -> Result<int, u8*> {
            if (c >= '0' && c <= '9') return Result::Ok((c - '0') as int);
            return Result::Err("not a digit");
        }
        fn add_digits(u8 a, u8 b) -> Result<int, u8*> {
            var x = parse_digit(a)?;
            var y = parse_digit(b)?;
            return Result::Ok(x + y);
        }
        fn find(int[] xs, int target) -> Option<int> {
            for (var i in 0..xs.len)
                if (xs[i] == target) return Option::Some(i as int);
            return Option::None;
        }
        enum Light { Red, Green }
        fn main() -> int {
            println(area(Shape::Circle(1.0)));
            println(area(Shape::Rect(2.0, 3.0)));
            println(area(Shape::Empty));
            Tree a = Tree::Leaf(3);
            Tree b = Tree::Leaf(4);
            Tree c = Tree::Node(&a, &b);
            println(sum(&c));
            match (add_digits('4', '5')) {
                Ok(v) => println(v);
                Err(e) => println(e);
            }
            match (add_digits('4', 'x')) {
                Ok(v) => println(v);
                Err(e) => println(e);
            }
            var r = find([5, 7, 9], 9);
            println(r.is_some());
            println(r.unwrap());
            println(find([1], 3).unwrap_or(-1));
            Option<int> none = Option::None;
            println(none.is_none());
            var o = Option::Some(2.5);
            match (o) {
                Some(x) => println(x);
                None => println("none");
            }
            Light l = Light::Green;
            match (l) {
                Red => println("stop");
                _ => println("go");
            }
            return 0;
        }
    )", "3.0\n6.0\n0.0\n7\n9\nnot a digit\ntrue\n2\n-1\ntrue\n2.5\ngo\n"));
}

TEST(e2e_variadics_and_format) {
    CHECK(prints("format", R"(
        import "std/io.ron";
        import "std/option.ron";
        struct Vec3 { float x, y, z; }
        enum Mood { Happy, Angry = 5 }
        fn total(..int nums) -> int {
            int s = 0;
            for (var n in nums) s += n;
            return s;
        }
        fn count(..Any things) -> i64 { return things.len; }
        fn main() -> int {
            println(total(1, 2, 3, 4));
            println(total());
            println(count("a", 1, 2.5, true));
            println("{} + {} = {}", 2, 3, 5);
            println("pos {} mood {}", Vec3{1.0, 2.5, -3.0}, Mood::Angry);
            println("{:.2} | {:x} | {:6}|", 3.14159, 255, 42);
            println("{} {} {}", "text", false, 'A');
            String s = format("{}-{}", 7, string("str"));
            println(s);
            int[3] arr = [1, 2, 3];
            int[] sl = arr[1..];
            println("{} {}", arr, sl);
            Option<int> o = Option::Some(1);
            println("{}", o);
            u64 big = 18446744073709551615;
            println("{{literal}} {}", big);
            println("{}", type_of(arr).name);
            return 0;
        }
    )", "10\n0\n4\n2 + 3 = 5\npos Vec3{x: 1.0, y: 2.5, z: -3.0} mood Angry\n3.14 | 0xff |     42|\ntext false 65\n7-str\n"
        "[1, 2, 3] [2, 3]\nSome\n{literal} 18446744073709551615\ni32[3]\n"));
}

void writeModule(const std::string& name, const std::string& source) {
    fs::path dir = fs::temp_directory_path() / "ron_e2e";
    fs::create_directories(dir);
    std::ofstream(dir / name, std::ios::binary) << source;
}

TEST(e2e_modules_and_global_init) {
    writeModule("geom.ron", R"(
        struct Point { int x, y; }
        private fn helper(int v) -> int { return v * 10; }
        fn scaled(int v) -> int { return helper(v); }
        fn init() -> int { return 1; }
        var origin_x = scaled(4);
    )");
    writeModule("sound.ron", R"(
        private fn helper(int v) -> int { return v + 1; }
        fn bump(int v) -> int { return helper(v); }
        fn init() -> int { return 2; }
    )");
    CHECK(prints("modules", R"(
        import "std/io.ron";
        import "geom.ron" as g;
        import "sound.ron" as s;
        struct Config { int w, h; }
        var counter_start = g::scaled(2) + 1;
        var config = Config{w: 640, h: 480};
        fn(int) -> int handler = s::bump;
        int[4] squares = [1, 4, 9, 16];
        fn main() -> int {
            println(g::init() + s::init());
            g::Point p = Point{1, 2};
            println(p.x + p.y);
            println(origin_x);
            println(counter_start);
            println(config.w * config.h);
            println(handler(5));
            println(squares[3]);
            println(g::origin_x);
            return 0;
        }
    )", "3\n3\n40\n21\n307200\n6\n16\n40\n"));

    RunResult priv = runProgram("modpriv", R"(
        import "geom.ron";
        fn main() -> int { return helper(1); }
    )");
    CHECK(!priv.compiled);
    CHECK(priv.errors.find("is private to another file") != std::string::npos);
    RunResult amb = runProgram("modamb", R"(
        import "geom.ron";
        import "sound.ron";
        fn main() -> int { return init(); }
    )");
    CHECK(!amb.compiled);
    CHECK(amb.errors.find("ambiguous") != std::string::npos);
}

TEST(e2e_runtime_errors) {
    const char* program = R"(
        struct Enemy { int hp; }
        fn hit(Enemy* e) -> int { return e.hp - 1; }
        fn update(int mode) -> int {
            int[3] arr = [1, 2, 3];
            int[] view = arr[..];
            var i = mode + 2;
            if (mode == 1) return arr[i];
            if (mode == 2) panic("boom");
            if (mode == 3) assert(mode == 4);
            if (mode == 4) return view[i + 5];
            Enemy* nothing = null;
            return hit(nothing);
        }
        fn main() -> int { return update(MODE); }
    )";
    auto run = [&](int mode, bool release = false) {
        std::string src = program;
        src.replace(src.find("MODE"), 4, std::to_string(mode));
        return runProgram("rt" + std::to_string(mode) + (release ? "r" : ""), src, release);
    };
    RunResult a = run(1);
    CHECK(a.output.find("index out of bounds: index 3, length 3 at rt1.ron:8") != std::string::npos);
    CHECK(a.output.find("at main (rt1.ron:15)") != std::string::npos);
    CHECK_EQ(a.exitCode, 101);
    RunResult b = run(2);
    CHECK(b.output.find("panic: boom at rt2.ron:9") != std::string::npos);
    RunResult c = run(3);
    CHECK(c.output.find("assertion failed: if (mode == 3) assert(mode == 4) at rt3.ron:10") != std::string::npos);
    RunResult d = run(4);
    CHECK(d.output.find("index 11, length 3") != std::string::npos);
    RunResult e = run(5);
    CHECK(e.output.find("crash: null pointer dereference in hit (rt5.ron:3)") != std::string::npos);
    CHECK(e.output.find("at update (rt5.ron:13)") != std::string::npos);
    RunResult rel = run(1, true);   // --release: no bounds checks
    CHECK(rel.compiled && rel.output.find("out of bounds") == std::string::npos);
}

TEST(e2e_warnings) {
    RunResult r = runProgram("warn", R"(
        fn f(u32 u) -> int {
            int unused = 3;
            if (u >= 0) return 1;
            return 0;
            int never = 2;
        }
        fn main() -> int { int _ok = 1; return f(1); }
    )");
    CHECK(r.compiled);
    CHECK_EQ(r.exitCode, 1);
    CHECK(r.errors.find("warning: unused variable 'unused'") != std::string::npos);
    CHECK(r.errors.find("warning: unreachable code") != std::string::npos);
    CHECK(r.errors.find("comparison is always true") != std::string::npos);
    CHECK(r.errors.find("'_ok'") == std::string::npos);
}

TEST(e2e_os_and_threads) {
    CHECK(prints("osthreads", R"(
        import "std/io.ron";
        import "std/os.ron";
        import "std/thread.ron";
        i64 counter = 0;
        i64 plain = 0;
        Mutex m;
        fn worker(void* arg) {
            for (var i in 0..10000) {
                atomic_add(&counter, 1);
                m.lock();
                plain += 1;
                m.unlock();
            }
        }
        fn main() -> int {
            List<Thread> threads;
            for (var i in 0..8) threads.push(spawn(worker, null));
            for (var t in &threads) t.join();
            println("{} {}", counter, plain);
            i64 v = 5;
            println(atomic_cas(&v, 5, 9));
            println(atomic_cas(&v, 5, 1));
            println(atomic_load(&v));
            println(cpu_count() > 0);
            var a = args();
            println(a.len >= 1);
            println(getenv("PATH").is_some());
            println(getenv("RON_SURELY_UNDEFINED_VARIABLE").is_none());
            var path = "ron_os_test.txt";
            write_file(path, "line1\n");
            append_file(path, string("line2\n"));
            match (read_text(path)) {
                Some(text) => print(text);
                None => println("read failed");
            }
            println(file_exists(path));
            delete_file(path);
            println(file_exists(path));
            return 0;
        }
    )", "80000 80000\ntrue\nfalse\n9\ntrue\ntrue\ntrue\ntrue\nline1\nline2\ntrue\nfalse\n"));
}

TEST(e2e_test_blocks) {
    RunResult r = runProgram("testblocks", R"(
        fn add(int a, int b) -> int { return a + b; }
        struct Box { int v; }
        test "addition" { assert(add(2, 3) == 5); }
        test "broken assert" { assert(add(2, 2) == 5); }
        test "crash" {
            Box* b = null;
            int x = b.v;
        }
        test "still runs after failures" { assert(true); }
        fn main() -> int { return 42; }   // ignored by the test runner
    )", false, true);
    CHECK(r.compiled);
    CHECK_EQ(r.exitCode, 1);
    CHECK(r.output.find("[ OK ] addition") != std::string::npos);
    CHECK(r.output.find("assertion failed: test \"broken assert\" { assert(add(2, 2) == 5); }") != std::string::npos);
    CHECK(r.output.find("[FAIL] broken assert") != std::string::npos);
    CHECK(r.output.find("crash: null pointer dereference") != std::string::npos);
    CHECK(r.output.find("[FAIL] crash") != std::string::npos);
    CHECK(r.output.find("[ OK ] still runs after failures") != std::string::npos);
    CHECK(r.output.find("2 passed, 2 failed") != std::string::npos);
    RunResult normal = runProgram("testblocks_main", R"(
        test "never compiled here" { this_does_not_exist(); }
        fn main() -> int { return 7; }
    )");
    CHECK(normal.compiled && normal.exitCode == 7);
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
