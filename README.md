# Machismo — Mach-O Loader for aarch64 Linux

Machismo loads **Apple Silicon (arm64) Mach-O binaries** on **aarch64 Linux**. Since Apple Silicon Mac builds contain native aarch64 machine code, machismo can run them without CPU emulation — no Wine, no Box64, no Rosetta.

The key insight: if we load the Mach-O binary, set up its memory layout, and redirect its library dependencies to native Linux `.so` equivalents, the ARM instructions run natively on any aarch64 Linux system.

Based on components from the [Darling](https://github.com/darlinghq/darling) project (macOS translation layer for Linux), extracted and adapted for standalone use.

## How It Works

```
┌─────────────────────────────────────────────────┐
│ Mach-O Game Binary (arm64 native code)          │
└────────────┬────────────────────────────────────┘
             │ GOT (patched by resolver)
             ▼
┌─────────────────────────────────────────────────┐
│ Resolver — Chained Fixup Patcher                │
│  • Patches GOT entries via dlopen/dlsym         │
│  • C++ ctor/dtor ABI adapters                   │
│  • Mach-O dylib loading for Apple-ABI deps      │
│  • Stub pool for unresolved binds               │
└────────────┬────────────────────────────────────┘
             ▼
┌──────┬──────────┬──────────┬──────────┐
│Native│ Apple-ABI│libsystem │Trampoline│
│Linux │ libc++   │_shim.so  │ system   │
│.so   │ (.so.1)  │          │          │
└──────┴──────────┴──────────┴──────────┘
```

**Machismo handles the hard parts:**

- **Fat/universal binary support** — extracts the arm64 slice
- **Mach-O segment mapping** with correct memory protections
- **Chained fixup resolution** — patches the GOT to redirect dylib imports to native Linux `.so` libraries
- **C++ ABI translation** — Apple ARM64 constructors return `this`, Linux doesn't; machismo wraps calls with ABI adapters
- **`std::string` layout compatibility** — builds libc++ with Apple's alternate SSO layout
- **pthread ABI translation** — detects macOS mutex/condvar/rwlock signatures and reinitializes them for Linux
- **Zero-initializing malloc** — macOS malloc returns zeroed pages; shim wraps `malloc → calloc` to match
- **Mach-O dylib loading** — loads `.dylib` dependencies as Mach-O when native substitutes won't work (vtable layout differences, etc.)
- **Symbol trampolining** — redirects statically-linked library calls to native `.so` implementations via 4-byte branch islands
- **Game function replacements** — override specific game functions with native implementations via `override_lib` config
- **ARMv8.0 compatibility** — emulates ARMv8.1 LSE atomics and downgrades ARMv8.3 RCPC instructions for older cores
- **C++ exception handling** — converts Apple compact unwind to DWARF `.eh_frame`
- **KMSDRM display** — auto-detects KMSDRM, forces fullscreen, shares SDL's EGL context with bgfx
- **Thread-local variables** — implements `_tlv_bootstrap` for Mach-O TLV descriptors
- **GDB debug symbols** — registers Mach-O symbols with GDB's JIT interface for readable backtraces
- **Binary patching** — applies instruction-level patches from config files
- **Commpage emulation** — maps the macOS commpage at `0xFFFFFC000`
- **libSystem shim** — maps Apple `libSystem.B.dylib` functions to glibc equivalents

## Building

### Prerequisites

- aarch64 Linux (native, not cross-compiled)
- GCC or Clang
- CMake 3.16+
- For external library builds: Ninja, clang++ (libc++), standard dev packages

### Build the loader

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

This produces:
- `machismo` — the Mach-O loader executable
- `libsystem_shim.so` — Apple libSystem.B.dylib compatibility shim
- `libgame_patches.so` — game-specific function replacements (NecroDancer)
- `wrapgen` — ELF wrapper code generator (utility)

### Build external libraries (game-specific)

These scripts build native Linux libraries with Apple ABI compatibility. Run from the machismo project root:

```bash
# Apple-ABI libc++ (required for any C++ Mach-O binary)
./scripts/build-libcxx.sh
cd build-libcxx && ninja cxx cxxabi && cd ..

# bgfx with OpenGL ES backend (for games using bgfx)
./scripts/build-bgfx.sh

# SFML 2.5.1 with Apple-ABI libc++ (for games using SFML)
./scripts/build-sfml.sh

# LuaJIT 2.1 with profiler integration (for games using LuaJIT)
./scripts/build-luajit.sh
```

## Usage

```bash
./machismo /path/to/macho-binary [args...]
```

Machismo looks for a config file in this order:
1. `$MACHISMO_CONFIG` environment variable
2. `machismo.conf` in the current directory
3. `machismo.conf` next to the binary

### Configuration

Machismo uses an INI-style config file:

```ini
[general]
dylib_map = dylib_map.conf
patches = patches/game.conf

[trampoline.sdl2]
lib = libSDL2-2.0.so.0
prefix = _SDL_

[trampoline.bgfx]
lib = ./build-bgfx/libbgfx-shared.so
prefix = _bgfx_
prefix = __ZN4bgfx
init_wrapper = true
renderer = opengles

[trampoline.steam_noop]
lib = STUB
prefix = __ZN3wos5steam

[trampoline.game]
override_lib = ./libgame_patches.so
```

### Dylib mapping

The `dylib_map.conf` file maps macOS dylib names to Linux equivalents:

```conf
# Map to native Linux .so
libsfml-graphics = ./build-sfml/lib/libsfml-graphics.so
libz.1 = libz.so

# Load as Mach-O dylib (preserves Apple ABI)
libsfml-audio = MACHO:./libsfml-audio.2.5.dylib

# Stub out (all symbols return 0)
libsteam_api = STUB

# Skip (leave symbols unresolved)
Carbon = SKIP
```

### Environment variables

| Variable | Purpose |
|----------|---------|
| `MACHISMO_CONFIG` | Path to config file (set to `none` to disable) |
| `MACHISMO_HOME` | Override game userdata/save location |
| `MACHISMO_DYLIB_MAP` | Path to dylib mapping file |
| `MACHISMO_PATCHES` | Path to binary patches file |
| `MACHISMO_TRAMPOLINE_LIB` | Native .so for symbol trampolining |
| `MACHISMO_TRAMPOLINE_PREFIX` | Symbol prefix for trampolining |
| `MACHISMO_VERBOSE_BINDS` | Log all bind resolutions |
| `MACHISMO_LUA_PROFILE` | Enable LuaJIT `jit.p` profiler, write output to given path |
| `MACHISMO_LUA_JITV` | Enable JIT verbose logging (trace compile/abort) |

## Testing

```bash
cd build && make
cd .. && bash tests/fixtures/build_fixtures.sh
bash tests/run_tests.sh
```

## Project Structure

```
machismo/
├── src/                    Core loader source
│   ├── machismo.c          Main entry point
│   ├── loader.c/h          Mach-O parsing and segment mapping
│   ├── stack.c             Mach-O stack layout setup
│   ├── commpage.c/h        macOS commpage emulation
│   ├── macho_defs.h        Mach-O structure definitions
│   ├── resolver.c/h        Chained fixup resolution
│   ├── dylib_loader.c/h    Mach-O dylib loading
│   ├── trampoline.c/h      Symbol trampolining system
│   ├── patcher.c/h         Binary instruction patcher
│   ├── gdb_jit.c/h         GDB JIT debug symbol registration
│   ├── eh_frame.c/h        Compact unwind → DWARF conversion
│   ├── lse_emul.c/h        ARMv8.1 LSE atomic emulation
│   ├── lua_entity_opt.c/h  Lua ECS metamethod optimization
│   ├── config.c/h          INI config file parser
│   ├── bgfx_shim.c/h       bgfx renderer integration shim
│   ├── stubs/              Apple ABI type definitions
│   ├── elfcalls/           ELF bridging interface
│   └── shim/               libSystem.B.dylib compatibility shim
├── scripts/                External library build scripts
├── patches/                Build-time patches (libc++, bgfx)
├── tools/                  Utility programs
├── tests/                  Test suite
├── examples/               Game configs + game-specific patches source
├── extern/                 Git submodules (bgfx, bimg, bx, llvm-project, sfml, LuaJIT)
└── CMakeLists.txt
```

## Key ABI Differences Handled

| Issue | macOS ARM64 | Linux ARM64 | Machismo Fix |
|-------|-------------|-------------|--------------|
| Constructor return | Returns `this` in x0 | Returns void | ABI adapter trampolines |
| `std::string` layout | Alternate SSO | Standard SSO | Apple-ABI libc++ build |
| `pthread_mutex_t` init | Signature `0x32AAABA7` | All zeros | Three-layer detection & fixup |
| `pthread_cond_t` init | Signature `0x3CB0B1BB` | All zeros | Detection in libc++ + shim |
| `malloc` zero-init | Returns zeroed pages | May return stale data | `malloc → calloc` wrapper in shim |
| Weak symbols | Searches main executable | dlsym only | Mach-O symbol table lookup |
| `__init_offsets` | Run by dyld | Not automatic | Explicit execution |
| TLV descriptors | `_tlv_bootstrap` thunk | `__thread` keyword | Custom `_tlv_bootstrap` |
| Exception handling | Compact unwind | DWARF `.eh_frame` | Compact → DWARF converter |
| `uint64_t` mangling | `y` (unsigned long long) | `m` (unsigned long) | Mangling fallback |
| RCPC (LDAPR) | ARMv8.3 baseline | ARMv8.0 on Cortex-A35 | In-place bit flip to LDAR |
| LSE atomics | ARMv8.1 baseline | ARMv8.0 on Cortex-A35 | Branch islands with LDXR/STXR loops |

## License

This project is licensed under the GNU General Public License v3.0 — see [LICENSE](LICENSE).

Based on [Darling](https://github.com/darlinghq/darling) by Lubos Dolezel and contributors.
