# Ron

A compiled programming language for 3D games, built completely from scratch.

`ronc` turns `.ron` source files into native Windows x86-64 executables. There is no LLVM,
no external assembler and no external linker: the compiler encodes every machine instruction
itself and writes the PE (`.exe`) file byte by byte.

```ron
import "std/io.ron";

struct Vec3 { float x, y, z; }

fn +(Vec3 a, Vec3 b) -> Vec3 { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
fn dot(Vec3 a, Vec3 b) -> float { return a.x * b.x + a.y * b.y + a.z * b.z; }

fn main() -> int {
    Vec3 v = Vec3{1.0, 2.0, 3.0};
    Vec3 w = v + v;
    println(w.dot(v));     // method-call syntax: dot(w, v)
    return 0;
}
```

See [docs/PLAN.md](docs/PLAN.md) for the architecture and roadmap.

## Building

Requirements: Windows 10/11 x64, Visual Studio 2022 Build Tools (MSVC + CMake).
The Vulkan SDK is needed for the engine (`ron_engine.dll`).

```powershell
.\build.ps1            # Debug build + tests
.\build.ps1 -Release
```

## Using ronc

```powershell
.\build\Debug\ronc.exe build game.ron            # -> game.exe
.\build\Debug\ronc.exe run game.ron              # build and run
.\build\Debug\ronc.exe run game.ron --watch      # rebuild + restart on every save
.\build\Debug\ronc.exe check game.ron            # type-check only
.\build\Debug\ronc.exe build game.ron --gui      # no console window
```

## Project layout

```
src/        compiler: lexer, parser, sema, x86-64 encoder, codegen, PE writer, driver
std/        standard library, written in Ron
tests/      unit and end-to-end tests (self-contained test framework)
docs/       design notes
```
