# Changelog

## 0.1.0

First release: a complete language, compiler, standard library and game engine, built from scratch.

### Language
- C-style syntax with structs, plain enums, pointers, fixed arrays and slices (`T[]`)
- Type inference (`var`, inferred `const`), untyped numeric literals
- Functions as values (`fn(A) -> R`), overloading, operator overloading, method-call syntax
- Generic structs, enums and functions with type deduction
- Sum types with payloads, `match` with bindings and exhaustiveness checks
- `Option<T>` / `Result<T, E>` and the `?` operator
- `switch`, `do`/`while`, labeled `break`/`continue`, `defer`, `for (x in items)` and ranges
- Array literals, named struct initializers, computed global initializers
- Variadic parameters (`..T`, `..Any`) with runtime type information and `type_of`
- Modules: `private` declarations, `import "x.ron" as m;` with `m::name`
- `test "name" { ... }` blocks

### Compiler (`ronc`)
- Native x86-64 code generation with register allocation; PE executables without external tools
- Dead-code elimination, constant folding, loop alignment
- Bounds checks in default builds, `--release` without them; warnings and `--Werror`
- Crash reports with stack traces, `panic`, `assert`
- `ronc build | run [--watch] | test | check | new | --version`

### Standard library
- `String`, `List<T>`, `Map<K, V>`, `Option`, `Result`, formatting (`format`, `println("{}", x)`)
- Math (vectors, matrices, trigonometry), random, time, OS (args, env, files, directories),
  threads (spawn, join, mutex, atomics)
- Rigid-body physics, entity component system, audio, engine bindings

### Engine (`ron_engine.dll`)
- Vulkan forward renderer: 4x MSAA, shadow mapping with PCF, PBR materials, fog, ACES tone mapping
- Textures with mipmaps (PNG decoder from scratch), glTF 2.0 models, GPU skinning and animation blending
- Window, keyboard and mouse input, software audio synthesizer, GPU screenshots

### Tooling
- VS Code syntax highlighting (`editors/vscode`)
