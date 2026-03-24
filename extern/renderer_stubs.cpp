#include <stdint.h>
#include <cstddef>
/*
 * Stubs for disabled bgfx renderers.
 * bgfx's dispatch table references rendererCreate/rendererDestroy for ALL
 * backends even when they're compiled out. Provide no-op stubs.
 */

namespace bx { class ReaderSeekerI; class Error; }

namespace bgfx {

struct Init;
struct RendererContextI;

#define STUB_RENDERER(ns) \
    namespace ns { \
        RendererContextI* rendererCreate(const Init&) { return nullptr; } \
        void rendererDestroy() {} \
    }

STUB_RENDERER(agc)
STUB_RENDERER(d3d9)
STUB_RENDERER(d3d11)
STUB_RENDERER(d3d12)
STUB_RENDERER(gnm)
STUB_RENDERER(nvn)
STUB_RENDERER(vk)
STUB_RENDERER(webgpu)

/* DXBC/DX9BC shader stubs — source files excluded to avoid X11 None macro conflict */
struct DxbcContext;
struct DxbcShader;
struct DxbcInstruction;
struct Dx9bcShader;
struct Dx9bcInstruction;
struct Dx9bc;

int32_t read(bx::ReaderSeekerI*, DxbcContext&, bx::Error*) { return 0; }
int32_t read(bx::ReaderSeekerI*, Dx9bc&, bx::Error*) { return 0; }

bool parse(const DxbcShader&,
           bool(*)(uint32_t, const DxbcInstruction&, void*),
           void*, bx::Error*) { return false; }

bool parse(const Dx9bcShader&,
           bool(*)(uint32_t, const Dx9bcInstruction&, void*),
           void*, bx::Error*) { return false; }

int32_t toString(char*, int32_t, const DxbcInstruction&) { return 0; }
int32_t toString(char*, int32_t, const Dx9bcInstruction&) { return 0; }

} // namespace bgfx
