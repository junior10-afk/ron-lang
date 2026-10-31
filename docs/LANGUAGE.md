# The Ron Language Reference (0.1)

Ron is a statically typed, compiled language with C-style syntax, designed for writing
games. `ronc` compiles it to native Windows x86-64 executables.

Contents: [Program structure](#program-structure) · [Types](#types) · [Literals](#literals) ·
[Variables](#variables-and-constants) · [Functions](#functions) · [Generics](#generics) ·
[Structs](#structs) · [Enums and sum types](#enums-and-sum-types) · [Statements](#statements) ·
[Operators](#operators) · [Slices](#slices) · [Error handling](#error-handling) ·
[Variadics and formatting](#variadic-functions-and-formatting) · [Modules](#modules) ·
[Runtime errors](#runtime-errors) · [Testing](#testing) · [FFI](#calling-c-and-windows) ·
[Standard library](#standard-library) · [Compiler](#compiler)

## Program structure

```ron
import "std/io.ron";

fn main() -> int {           // the return value is the process exit code
    println("Hello, {}!", "world");
    return 0;
}
```

`main` may also return nothing (`fn main() { ... }`). Global variables are initialized before
`main` runs, in import order.

## Types

| Type | Description |
|------|-------------|
| `bool` | `true` / `false` |
| `i8` `i16` `i32` `i64`, `u8` `u16` `u32` `u64` | integers |
| `f32` `f64` | floating point |
| `void` | no value (function results, `void*`) |
| `T*` | pointer |
| `T[N]` | fixed-size array (`N` is a literal or an integer `const`) |
| `T[]` | slice: pointer + length |
| `fn(A, B) -> R` | function value |
| `struct`, `enum` | user-defined, optionally generic: `List<T>`, `Option<T>` |

Aliases: `int` = `i32`, `uint` = `u32`, `float` = `f32`, `double` = `f64`, `char` = `byte` = `u8`,
`usize` = `u64`, `isize` = `i64`.

## Literals

```ron
42   1_000_000   0xFF   0b1010        // integers
1.5   3e2   1.5e-3   0.25f            // floats (f suffix = f32)
'a'   '\n'   'é'                      // characters (integer code point)
"text\n"   "\u{1F680}"                // strings (u8*, null-terminated)
true   false   null
[1, 2, 3]                             // array literal
Vec3{1.0, 2.0, 3.0}   Vec3{x: 1.0}    // struct literals (omitted fields are zero)
```

Number literals are **untyped** until context gives them a type: `float f = 1;`, `u8 b = 255;` and
`x * 0.5` need no casts. A literal that does not fit its target type is a compile error.

## Variables and constants

```ron
int lives = 3;               // locals are zero-initialized when no value is given
var speed = 4.5;             // type inferred (here double); integer literals infer int
const int MAX = 64;          // constants are folded at compile time
float[MAX] samples;
var table = [1, 2, 4, 8];    // int[4]
```

Top-level `var` / typed globals can be initialized with any expression (function calls, struct
literals, function values); `const` needs a compile-time value.

## Functions

```ron
fn add(int a, int b) -> int { return a + b; }
fn log(u8* message) { ... }
```

- **Overloading** by parameter types; the best match wins (exact > literal > closest widening).
- **Method-call syntax**: `v.length()` calls `length(v)`; `player.update(dt)` passes `&player`
  automatically when the first parameter is a pointer.
- **Operator overloading** on structs: `fn +(Vec3 a, Vec3 b) -> Vec3`, `fn -(Vec3 a)`, `fn ==`, `fn <` …
- **Function values**: functions are values of type `fn(...) -> R`. They can be stored in variables
  and struct fields, passed as callbacks, compared with `null` and called like functions:

```ron
fn apply(fn(int) -> int f, int v) -> int { return f(v); }
struct Button { u8* label; fn() on_click; }
fn(int, int) -> int op = add;      // the overload matching the type is chosen
```

Built-in intrinsics: `sqrt(x)`, `type_of(x)`, `atomic_add/cas/load/store`, `assert(c)`, `panic(msg)`.

## Generics

Structs, enums and functions take type parameters; every combination of type arguments is
compiled separately (monomorphization).

```ron
struct Pair<A, B> { A first; B second; }
fn swap<T>(T* a, T* b) { T t = *a; *a = *b; *b = t; }
fn max_of<T>(T a, T b) -> T { return a > b ? a : b; }

swap(&x, &y);                        // type arguments deduced from the arguments
List<List<int>> grid;
var z = zero::<float>();             // explicit type arguments
Pair<int, float> p = Pair::<int, float>{1, 2.5};
```

A generic body is checked when instantiated, so it may use any operation its type arguments
support. A non-generic overload wins over a generic one when both match equally.

## Structs

```ron
struct Vec3 { float x, y, z; }
struct Player { Vec3 position; int health; Player* target; }

Player* p = &player;
p.health -= 10;                 // `.` auto-dereferences pointers
```

Structs are values (copied on assignment and when passed); layout follows C rules.

## Enums and sum types

Plain enums are named integers:

```ron
enum State { Idle, Running = 10, Done }
```

An enum whose variants carry values is a **sum type** (tagged union), optionally generic and
recursive through pointers:

```ron
enum Shape { Circle(float), Rect(float, float), Empty }
enum Tree { Leaf(int), Node(Tree*, Tree*) }

Shape s = Shape::Circle(2.0);

match (s) {
    Circle(r) => println("circle {}", r);
    Rect(w, h) => println("{}x{}", w, h);
    _ => println("something else");
}
```

`match` binds payload values to names (`_` ignores one) and must handle every variant unless it
has a `_` arm. It also works on plain enums.

## Statements

```ron
if (x > 0) { ... } else if (x < 0) { ... } else { ... }
while (running) { ... }
do { ... } while (again);
for (int i = 0; i < 10; i++) { ... }
for (var item in items) { ... }        // arrays, slices, List<T>
for (var p in &items) { p.hp -= 1; }   // pointers to the elements
for (int i in 0..n) { ... }            // ranges (end excluded)
switch (key) {
case 1, 2: ...                        // no fallthrough; `break` leaves the switch
case KEY_ESCAPE: ...
default: ...
}
outer: for (...) { for (...) { if (done) break outer; } }
defer close(file);                    // runs when the block exits, even through return/break
```

Conditions must be `bool`. A `switch` on an enum must handle every member or have `default`.
Deferred statements run in reverse order and cannot `return` or jump out.

## Operators

Lowest to highest precedence:

| Operators | Notes |
|-----------|-------|
| `= += -= *= /= %= &= \|= ^= <<= >>=` | right associative |
| `c ? a : b` | conditional |
| `\|\|`, `&&` | short-circuit |
| `\|` `^` `&` | bitwise |
| `== !=`, `< <= > >=` | comparisons |
| `<< >>` | `>>` is arithmetic for signed types |
| `+ -`, `* / %` | pointer ± integer scales by the element size |
| `x as T` | explicit conversion |
| `- ! ~ & * ++ --` | unary prefix |
| `f() a[i] a[lo..hi] a.b x? ++ --` | postfix |

Implicit conversions: value-preserving integer widening, integer to float, `f32` to `f64`,
`null` to pointers and function values, pointers to/from `void*`, arrays to slices and to a
pointer to their first element. Everything else needs `as`.

## Slices

`T[]` is a view `{ T* ptr; i64 len }` over contiguous elements:

```ron
fn sum(int[] xs) -> int { int s = 0; for (var x in xs) s += x; return s; }

int[5] a = [1, 2, 3, 4, 5];
sum(a);              // arrays convert to slices
sum(a[1..4]);        // 2 + 3 + 4
sum([10, 20]);       // array literals too
int[] all = list.slice();
```

`a[lo..hi]`, `a[lo..]`, `a[..hi]`, `a[..]` slice arrays, slices and (with an end) pointers.

## Error handling

`std/option.ron` defines `Option<T> { Some(T), None }` and `Result<T, E> { Ok(T), Err(E) }`.
The `?` operator unwraps a value or returns the `None` / `Err` from the current function:

```ron
fn parse_digit(u8 c) -> Result<int, u8*> {
    if (c >= '0' && c <= '9') return Result::Ok((c - '0') as int);
    return Result::Err("not a digit");
}

fn add_digits(u8 a, u8 b) -> Result<int, u8*> {
    var x = parse_digit(a)?;
    var y = parse_digit(b)?;
    return Result::Ok(x + y);
}
```

`Option::None` / `Result::Err(...)` take their type arguments from where they are used.
Helpers: `unwrap`, `unwrap_or`, `expect`, `is_some`, `is_none`, `is_ok`, `is_err`, `unwrap_err`, `ok`.

## Variadic functions and formatting

A final `..T` parameter receives the extra arguments as a `T[]` slice. With `..Any` each argument
is boxed with its runtime type information (`TypeInfo`: kind, size, name, fields, members…):

```ron
fn total(..int nums) -> int { ... }
fn log(u8* fmt, ..Any args) { ... }
```

`std/fmt.ron` (imported by `std/io.ron`) formats every type, including structs field by field:

```ron
println("{} hp at {}", player.hp, player.position);   // 90 hp at Vec3{x: 1.0, y: 0.0, z: 2.0}
String s = format("{:.2} ms | {:x} | {:6}", dt, flags, score);
```

Placeholders: `{}`, `{:.N}` (decimals), `{:x}` (hexadecimal), `{:W}` (right-aligned width), `{{` `}}`.

## Modules

Every file is a module. `import "path.ron";` makes its public declarations available. A
`private` declaration is visible only inside its file. `import ... as m;` adds qualified access:

```ron
import "physics.ron" as phys;
import "audio.ron" as audio;

phys::init();               // both modules define init(): qualify the call
phys::Body b;
var gravity = phys::GRAVITY;
```

Unqualified calls that match functions from several modules equally are reported as ambiguous;
declarations of the calling file take precedence over imported ones.

## Runtime errors

Default builds check array and slice indexes. Failures and crashes print the location and the
call stack, then exit:

```
index out of bounds: index 3, length 3 at game.ron:42
stack trace:
  at update (game.ron:42)
  at main (game.ron:80)

crash: null pointer dereference in hit (game.ron:12)
```

`panic("message")` and `assert(condition)` report the same way. `ronc build --release` removes
the bounds checks.

## Testing

```ron
fn add(int a, int b) -> int { return a + b; }

test "add works" {
    assert(add(2, 3) == 5);
}
```

`ronc test file.ron` builds the file's test blocks with a generated `main` and runs each test on
its own thread, so a failed assertion or a crash fails only that test. Normal builds ignore tests.

## Calling C and Windows

```ron
extern "kernel32.dll" {
    fn Sleep(u32 milliseconds);
}
private extern "user32.dll" fn MessageBoxA(void* hwnd, u8* text, u8* caption, u32 type) -> i32;
```

Calls use the Windows x64 calling convention; Ron functions can be passed to C as callbacks.

## Standard library

| Module | Contents |
|--------|----------|
| `std/io.ron` | `print`, `println` (also formatted), `read_file` |
| `std/fmt.ron` | `format`, `format_into`, placeholders for every type |
| `std/string.ron` | `String` (`+`, `==`, `append`, `find`, `substring`, `to_upper`, `parse_int`), `strlen`, `streq`, `StrBuf` |
| `std/list.ron` | `List<T>`: `push`, `pop`, `insert`, `remove_at`, `remove_swap`, `sort`, `index_of`, `slice` |
| `std/map.ron` | `Map<K, V>`: `put`, `get`, `has`, `remove` |
| `std/option.ron` | `Option<T>`, `Result<T, E>` |
| `std/mem.ron` | `alloc`, `realloc`, `free`, `memcpy`, `memset` |
| `std/math.ron` | trigonometry, `Vec2/3/4`, `Mat4`, interpolation |
| `std/os.ron` | `args`, `getenv`, files, directories, `exit` |
| `std/thread.ron` | `spawn`, `join`, `Mutex`, `cpu_count` |
| `std/time.ron`, `std/random.ron` | clocks, random numbers |
| `std/physics.ron`, `std/ecs.ron` | rigid bodies, entity component system |
| `std/engine.ron`, `std/audio.ron` | Vulkan rendering, PBR materials, textures, glTF models, animation, input, sound |

## Compiler

```
ronc build game.ron [-o game.exe] [--gui] [--release] [--Werror]
ronc run   game.ron [--watch]       build and run; --watch rebuilds and restarts on every save
ronc test  game.ron                 run the test blocks
ronc check game.ron                 type-check only
ronc new   folder                   create a project
ronc --version
```

Warnings: unused variables (prefix a name with `_` to silence), unreachable code, comparisons that
are always true or false. Only functions reachable from `main` are compiled.
