# Ron — Implementation Plan

Ron is a compiled language for 3D games, built from scratch: no LLVM, no external
assembler, no external linker. `ronc` turns `.ron` source into a native Windows x86-64
`.exe` by writing machine code and the PE file itself.

## Pipeline

```
source.ron ─► Lexer ─► Parser ─► AST ─► Sema (types) ─► x86-64 CodeGen ─► PE writer ─► game.exe
                                                                                          │
                                         ron_engine.dll (C++ / Vulkan) ◄── imports ───────┘
```

## Milestones

| # | Milestone | Deliverable |
|---|-----------|-------------|
| 1 | Lexer | Tokens, diagnostics with source excerpts |
| 2 | Parser + AST | Recursive descent, precedence climbing, error recovery |
| 3 | Semantic analysis | Type checking, overloads, implicit conversions, UFCS method calls |
| 4 | x86-64 code generator | Hand-written instruction encoder, Win64 calling convention, SSE floats |
| 5 | PE writer | `.text` / `.rdata` / `.data` sections, import tables, runnable `.exe` |
| 6 | Runtime & standard library | Written in Ron: I/O, memory, strings, math (sin/cos/sqrt), vectors/matrices |
| 7 | Vulkan engine | `ron_engine.dll`: window, swapchain, depth, lit meshes, camera, input |
| 8 | Game features | ECS, physics (gravity, AABB/sphere collisions), audio, `ronc --watch` hot reload |

## Key design decisions

- **C-style syntax**: `int x = 5;`, `fn add(int a, int b) -> int { ... }`, braces and semicolons.
  Types use prefix notation: `float[16] m;`, `Vec3* p;`.
- **Types**: `bool`, `i8..i64`, `u8..u64`, `int` (= `i32`), `uint` (= `u32`), `float` (= `f32`),
  `double` (= `f64`), pointers, fixed arrays, structs, enums.
- **Pointers auto-deref on member access**: `p.x` works for `Vec3* p`.
- **Overloading and UFCS**: `len(v)` can be written `v.len()`. Operators can be overloaded:
  `fn +(Vec3 a, Vec3 b) -> Vec3`.
- **Modules**: `import "std/math.ron";` (deduplicated, one global namespace).
- **FFI**: `extern "kernel32.dll" { fn ExitProcess(u32 code); }` — calls go through the PE
  import address table.
- **Codegen strategy**: single pass over the typed AST. Expression results live in `rax`
  (integers, pointers, struct addresses) or `xmm0` (floats). Temporaries use frame slots, so `rsp`
  stays 16-byte aligned for calls. Win64 ABI everywhere, including Ron-to-Ron calls.
- **No relocations needed**: all code/data references are RIP-relative.
- **Engine**: C++ DLL with a flat C API, so Ron calls it through the same FFI as any DLL.
