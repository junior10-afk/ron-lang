# The Ron Language Reference

Ron is a statically typed, compiled language with C-style syntax, designed for
writing games. This page describes the language as implemented by `ronc` 0.1.

## Program structure

A program is a set of `.ron` files. Execution starts at `main`:

```ron
fn main() -> int {     // the return value is the process exit code
    return 0;
}
```

`main` may also return nothing (`fn main() { ... }`), which exits with code 0.

### Imports

```ron
import "std/io.ron";       // standard library (shipped next to ronc.exe)
import "player.ron";       // relative to the importing file
```

Each file is loaded once, whatever the number of imports. All declarations share
one global namespace.

## Comments

```ron
// line comment
/* block comment /* they nest */ still a comment */
```

## Types

| Type | Description |
|------|-------------|
| `bool` | `true` / `false` |
| `i8` `i16` `i32` `i64` | signed integers |
| `u8` `u16` `u32` `u64` | unsigned integers |
| `f32` `f64` | floating point |
| `void` | no value (function results, `void*`) |
| `T*` | pointer to `T` |
| `T[N]` | fixed-size array (`N` is a literal or an integer `const`) |
| `struct` / `enum` types | user-defined |

Aliases: `int` = `i32`, `uint` = `u32`, `float` = `f32`, `double` = `f64`,
`char` = `byte` = `u8`, `usize` = `u64`, `isize` = `i64`.

## Literals

```ron
42   1_000_000   0xFF   0b1010        // integers
1.5   3e2   1.5e-3   0.25f            // floats (f suffix = f32)
'a'   '\n'   'é'                      // characters (integer code point)
"text\n"   "\u{1F680}"                // strings (type u8*, null-terminated)
true   false   null
```

Escapes: `\n \t \r \0 \\ \" \' \xHH \u{...}`. Adjacent string literals are
concatenated.

Number literals are **untyped** until context gives them a type, so `float f = 1;`,
`u8 b = 255;` and `x * 0.5` all work without casts. A literal that does not fit its
target type is a compile error.

## Variables and constants

```ron
int lives = 3;              // locals are zero-initialized when no value is given
float[16] matrix;           // array
Vec3* target = null;
const float GRAVITY = 9.81; // const locals cannot be reassigned
```

Top-level declarations:

```ron
int score = 0;              // global variable (initializer must be a constant)
const int MAX_ENEMIES = 64; // compile-time constant, usable as an array size
```

## Functions

```ron
fn add(int a, int b) -> int { return a + b; }
fn log(u8* message) { ... }             // no return type = void
```

- **Overloading**: several functions may share a name if their parameter types differ.
  The best match is chosen (exact > literal > widening conversion).
- **Method-call syntax (UFCS)**: `v.length()` calls `length(v)`. If the first parameter is a
  pointer, the address is taken automatically: `player.update(dt)` calls `update(&player, dt)`.
- **Operator overloading** on structs:

```ron
fn +(Vec3 a, Vec3 b) -> Vec3 { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
fn -(Vec3 a) -> Vec3 { return Vec3{-a.x, -a.y, -a.z}; }   // unary minus
fn ==(Vec3 a, Vec3 b) -> bool { ... }
```

Overloadable: `+ - * / % == != < <= > >= & | ^`. Compound assignments (`+=` …) use them too.

Built-in intrinsic: `sqrt(x)` compiles to a single SSE instruction.

## Generics

Structs and functions can take type parameters. Each combination of type arguments is
compiled to its own specialized code (monomorphization), so generic code runs as fast as
hand-written code.

```ron
struct Pair<A, B> { A first; B second; }
struct Node<T> { T value; Node<T>* next; }       // recursive through a pointer

fn swap<T>(T* a, T* b) { T t = *a; *a = *b; *b = t; }
fn max_of<T>(T a, T b) -> T { return a > b ? a : b; }

Pair<int, float> p;
swap(&x, &y);                        // T is deduced from the arguments
int m = max_of(4, 9);
List<List<int>> grid;                // nested instances
float z = zero::<float>();           // explicit type arguments when nothing can be deduced
Pair<int, float> q = Pair::<int, float>{1, 2.5};
```

A generic body is checked when it is instantiated, so it may use any operation the type
arguments support (`a > b` works for numbers and for structs that overload `>`). When both a
generic and a non-generic function match equally well, the non-generic one wins.

## Structs

```ron
struct Vec3 { float x, y, z; }
struct Player {
    Vec3 position;
    int health;
    Player* target;     // pointers to the same struct are fine
}

Vec3 v = Vec3{1.0, 2.0, 3.0};   // positional struct literal
Vec3 zero = Vec3{};             // zero-initialized
Player* p = &player;
p.health -= 10;                 // `.` auto-dereferences pointers
```

Structs are values: assignment and parameter passing copy them. Layout follows C rules
(natural alignment), so structs can be shared with C/C++ code.

## Enums

```ron
enum State { Idle, Running = 10, Done }   // Done = 11
State s = State::Running;
int raw = s as int;
```

Enum values convert implicitly to integers, not the other way around.

## Statements

```ron
if (x > 0) { ... } else if (x < 0) { ... } else { ... }
while (running) { ... }
for (int i = 0; i < 10; i++) { ... }
break;  continue;  return value;
```

Conditions must be `bool` (write `if (count != 0)`, not `if (count)`).

## Operators

From lowest to highest precedence:

| Operators | Notes |
|-----------|-------|
| `= += -= *= /= %= &= \|= ^= <<= >>=` | right associative |
| `c ? a : b` | conditional |
| `\|\|` | logical or (short-circuit) |
| `&&` | logical and (short-circuit) |
| `\|` `^` `&` | bitwise |
| `== !=` | |
| `< <= > >=` | |
| `<< >>` | `>>` is arithmetic for signed types |
| `+ -` | pointer + integer scales by the element size |
| `* / %` | |
| `x as T` | explicit conversion |
| `- ! ~ & * ++ --` | unary prefix |
| `f() a[i] a.b ++ --` | postfix |

Other expressions: `sizeof(T)`, `array.len` (element count of a fixed array).

### Conversions

Implicit: integer widening that preserves the value, any integer to float, `f32` to `f64`,
`null` to any pointer, any pointer to/from `void*`, array to pointer to its first element.

Everything else needs `as`: `f as int` (truncates), `i as u8` (wraps), `p as u64`, `x as float`.

## Interfacing with C / Windows

Functions from any DLL can be declared and called directly:

```ron
extern "kernel32.dll" {
    fn Sleep(u32 milliseconds);
    fn GetTickCount64() -> u64;
}
extern "user32.dll" fn MessageBoxA(void* hwnd, u8* text, u8* caption, u32 type) -> i32;
```

Calls follow the Windows x64 calling convention, so structs and floats are passed exactly
as C does.

## Standard library

| Module | Contents |
|--------|----------|
| `std/io.ron` | `print`, `println` (strings, `String`, integers, floats, bools), `read_file` |
| `std/string.ron` | growable `String` (`+`, `==`, `append`, `find`, `substring`, `to_upper`, `parse_int`), `strlen`, `streq`, number formatting, `StrBuf` |
| `std/list.ron` | `List<T>`: `push`, `pop`, `insert`, `remove_at`, `remove_swap`, `sort`, `index_of`, `contains` |
| `std/map.ron` | `Map<K, V>` hash map: `put`, `get`, `has`, `remove` (integer, pointer and `String` keys) |
| `std/mem.ron` | `alloc`, `realloc`, `free`, `memcpy`, `memset` |
| `std/math.ron` | `sin cos tan atan atan2 floor ceil fmod abs min max clamp lerp smoothstep`, `Vec2 Vec3 Vec4 Mat4` with operators, `dot cross normalize length distance`, `mat4_perspective mat4_look_at mat4_trs`... |
| `std/time.ron` | `now()` (seconds, high resolution), `sleep_ms` |
| `std/random.ron` | `random()`, `random_range`, `random_int`, `seed_random` |
| `std/physics.ron` | rigid bodies (spheres, boxes), gravity, collisions, contact events |
| `std/ecs.ron` | entity component system: `World`, `spawn`, components, systems |
| `std/engine.ron` | window, input, Vulkan rendering, PBR `Material`s, textures (PNG or procedural `Image`), glTF models, `Animator` (cross-faded skeletal animation), noise, camera helpers |
| `std/audio.ron` | synthesized sound effects and notes |
| `std/win32.ron` | raw Windows API imports |

## Compiler

```
ronc build game.ron [-o game.exe] [--gui]   native executable (--gui: no console window)
ronc run game.ron [--watch]                 build + run; --watch rebuilds and restarts on save
ronc check game.ron                         type-check only
ronc tokens game.ron                        dump the lexer output
```

Only functions reachable from `main` are compiled, and DLLs that ship next to `ronc.exe`
(like `ron_engine.dll`) are copied next to the produced executable.
