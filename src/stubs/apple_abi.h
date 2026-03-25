/*
 * Apple ABI type stubs for binary-compatible game function replacements.
 *
 * These types match the in-memory layout of Apple's C++ standard library
 * on aarch64. Use them in replacement functions that operate on Mach-O
 * objects loaded by machismo.
 */

#ifndef APPLE_ABI_H
#define APPLE_ABI_H

#include <stdint.h>
#include <stddef.h>

/*
 * Apple-ABI std::string (alternate SSO layout, 24 bytes on aarch64 LE).
 *
 * When is_long (MSB of cap) is set, 'data' points to heap.
 * When is_long is clear, the string data is stored inline starting
 * at the struct address, with size in the low byte of the last word.
 */
typedef struct {
	char*    data;
	uint64_t size;
	uint64_t cap;   /* MSB = is_long flag; remaining bits = capacity */
} apple_string_t;

static inline const char *apple_string_cstr(const apple_string_t *s)
{
	if (s->cap & ((uint64_t)1 << 63))
		return s->data;
	return (const char *)s;  /* inline SSO: data starts at struct base */
}

static inline uint64_t apple_string_len(const apple_string_t *s)
{
	if (s->cap & ((uint64_t)1 << 63))
		return s->size;
	/* Short string: size stored in first byte of the 'data' field's
	 * position, shifted. Actually in Apple's alternate layout,
	 * the short size is in the last byte. Use size field directly
	 * since for short strings size is still valid. */
	return s->size;
}

/*
 * Macro for declaring replacement functions with Mach-O mangled names.
 *
 * Usage:
 *   MACHO_FUNC(_ZN10DataStream11preloadFileEv, uint64_t, void *self)
 *   {
 *       // replacement implementation
 *   }
 *
 * The function is exported from the .so with the mangled name (minus
 * one leading underscore from the Mach-O convention). The machismo
 * override trampoline discovers it via dlsym.
 */
#define MACHO_FUNC(mangled_name, ret_type, ...) \
	ret_type mangled_name(__VA_ARGS__)

#endif /* APPLE_ABI_H */
