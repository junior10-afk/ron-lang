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

| # | Milestone | Deliverable | Status |
|---|-----------|-------------|--------|
| 1 | Lexer | Tokens, diagnostics with source excerpts | done |
| 2 | Parser + AST | Recursive descent, precedence climbing, error recovery | done |
| 3 | Semantic analysis | Type checking, overloads, implicit conversions, UFCS method calls | done |
| 4 | x86-64 code generator | Hand-written instruction encoder, Win64 calling convention, SSE floats | done |
| 5 | PE writer | `.text` / `.rdata` / `.data` sections, import tables, runnable `.exe` | done |
| 6 | Runtime & standard library | Written in Ron: I/O, memory, strings, math (sin/cos/sqrt), vectors/matrices | done |
| 7 | Vulkan engine | `ron_engine.dll`: window, swapchain, depth, MSAA, shadows, lit meshes, camera, input | done |
| 8 | Game features | ECS, physics (gravity, AABB/sphere collisions), audio, `ronc run --watch` live reload | done |
| 9 | Performance | Register allocation (callee-saved GPR/XMM), SIB addressing, immediates, branch fusion | done |
| 10 | Generics & containers | Monomorphized generic structs/functions, `List<T>`, `Map<K, V>`, `String` | done |
| 11 | Materials & textures | PBR (GGX), PNG decoder, mipmapped textures, world-space UVs | done |
| 12 | Animation | glTF 2.0 loader, GPU skinning, keyframe sampling, cross-fade blending | done |
| 13 | Syntax | `var`, `switch`, `defer`, `do-while`, labeled loops, array literals, named initializers | done |
| 14 | Functions & slices | Function values and callbacks, `T[]` slices, `for-in` loops and ranges | done |
| 15 | Sum types | Tagged unions, `match`, `Option` / `Result`, the `?` operator | done |
| 16 | Variadics | `..T` / `..Any`, runtime type information, `format` and `println("{}")` | done |
| 17 | Modules | `private`, `import ... as m`, computed global initializers | done |
| 18 | Runtime | Stack traces, `panic`, `assert`, bounds checks, warnings | done |
| 19 | System library | Files, directories, args, env, threads, mutexes, atomics | done |
| 20 | Tooling | `test` blocks, `ronc test/new/--version`, VS Code highlighting, docs, release 0.1.0 | done |
| 21 | Realistic characters | JPEG + HDR decoders, full PBR texture set, normal maps (tangents), image-based lighting, morph targets, alpha mask/blend, skin and hair shading | done |

## Further work (0.2 and later)

- **Compiler**: PDB debug info (step through Ron code in a debugger), x64 unwind tables, SIMD code
  generation for vector math, inlining, a language server for editors.
- **Language**: closures capturing variables, traits/interfaces, `match` as an expression,
  compile-time evaluation.
- **Engine**: instanced rendering, post-processing (bloom, SSAO, depth of field), cascaded
  shadows, screen-space subsurface scattering and strand-based hair, KTX2/Basis textures and
  meshopt compression, a UI layer, state-preserving hot reload.

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
