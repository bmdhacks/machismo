/*
 * test_necrolevel_rng.c — Verify libnecrolevel.dylib RNG produces varied output
 *
 * Loads libnecrolevel.dylib as Mach-O, finds the internal bb_random_Rnd()
 * function and _bb_random_Seed global, and checks that the RNG actually
 * produces different values across calls.
 *
 * This helps diagnose the "all enemies are green slime" bug — if the RNG
 * always returns the same value, enemy selection would always pick type 0.
 *
 * The RNG function is a pure LCG with no external dependencies, so it
 * works without resolving chained fixups.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dylib_loader.h"
#include "macho_defs.h"

/* Internal libnecrolevel symbols (LOCAL, not exported) — known vaddrs */
#define VADDR_BB_RANDOM_SEED  0x00498dcc   /* uint32_t global */
#define VADDR_BB_RANDOM_RND   0x0002096c   /* float bb_random_Rnd(void) */
#define VADDR_BB_RANDOM_RND2  0x00013f28   /* float bb_random_Rnd2(float lo, float hi) */

typedef float (*bb_random_Rnd_t)(void);
typedef float (*bb_random_Rnd2_t)(float lo, float hi);

/*
 * Look up a LOCAL symbol by scanning the full symtab (not just N_EXT).
 * Returns runtime address (vmaddr + slide) or 0.
 */
static uintptr_t dylib_lookup_local(struct macho_dylib_info *info, const char *name)
{
	if (!info || !info->symtab || !info->strtab)
		return 0;

	for (uint32_t i = 0; i < info->nsyms; i++) {
		struct nlist_64 *nl = &info->symtab[i];
		if (nl->n_type & 0xe0) continue;  /* skip N_STAB */
		if ((nl->n_type & 0x0e) != 0x0e) continue;  /* N_SECT only */
		if (nl->n_strx >= info->strsize) continue;

		const char *sym = &info->strtab[nl->n_strx];
		if (strcmp(sym, name) == 0)
			return nl->n_value + info->slide;
	}
	return 0;
}

static int test_rng_variation(struct macho_dylib_info *info)
{
	/* Find RNG function and seed by known vaddr offsets */
	bb_random_Rnd_t rng_func = (bb_random_Rnd_t)(info->slide + VADDR_BB_RANDOM_RND);
	uint32_t *seed_ptr = (uint32_t *)(info->slide + VADDR_BB_RANDOM_SEED);

	/* Also try finding by symbol name as validation */
	uintptr_t seed_by_sym = dylib_lookup_local(info, "__bb_random_Seed");
	uintptr_t rng_by_sym = dylib_lookup_local(info, "_bb_random_Rnd");

	printf("  seed addr (by vaddr): %p\n", (void *)(info->slide + VADDR_BB_RANDOM_SEED));
	printf("  seed addr (by symbol): %p\n", (void *)seed_by_sym);
	printf("  rng  addr (by vaddr): %p\n", (void *)(info->slide + VADDR_BB_RANDOM_RND));
	printf("  rng  addr (by symbol): %p\n", (void *)rng_by_sym);

	if (seed_by_sym && seed_by_sym != (uintptr_t)seed_ptr) {
		fprintf(stderr, "FAIL: seed address mismatch: symbol=%p vaddr=%p\n",
		        (void *)seed_by_sym, (void *)seed_ptr);
		return 1;
	}

	/* Set a known seed */
	*seed_ptr = 12345;
	printf("  initial seed: %u\n", *seed_ptr);

	/* Call RNG 10 times and check for variation */
	float values[10];
	uint32_t seeds[10];
	int unique_count = 0;

	for (int i = 0; i < 10; i++) {
		values[i] = rng_func();
		seeds[i] = *seed_ptr;
		printf("  call %d: value=%.6f  seed=%u\n", i, values[i], seeds[i]);

		/* Check this value differs from all previous */
		int is_unique = 1;
		for (int j = 0; j < i; j++) {
			if (values[i] == values[j]) {
				is_unique = 0;
				break;
			}
		}
		if (is_unique) unique_count++;
	}

	printf("  unique values: %d/10\n", unique_count);

	if (unique_count < 8) {
		fprintf(stderr, "FAIL: RNG produced only %d/10 unique values (expected >= 8)\n",
		        unique_count);
		return 1;
	}

	/* Verify seed actually mutates between calls */
	int seed_changes = 0;
	for (int i = 1; i < 10; i++) {
		if (seeds[i] != seeds[i-1])
			seed_changes++;
	}
	if (seed_changes < 9) {
		fprintf(stderr, "FAIL: seed only changed %d/9 times\n", seed_changes);
		return 1;
	}

	printf("  PASS: RNG produces varied output\n");
	return 0;
}

static int test_rng_zero_seed(struct macho_dylib_info *info)
{
	/* Test what happens when seed starts at 0 (BSS default) */
	bb_random_Rnd_t rng_func = (bb_random_Rnd_t)(info->slide + VADDR_BB_RANDOM_RND);
	uint32_t *seed_ptr = (uint32_t *)(info->slide + VADDR_BB_RANDOM_SEED);

	*seed_ptr = 0;
	printf("  seed=0 test:\n");

	float first = rng_func();
	uint32_t seed_after_first = *seed_ptr;
	float second = rng_func();

	printf("    first:  %.6f (seed now: %u)\n", first, seed_after_first);
	printf("    second: %.6f (seed now: %u)\n", second, *seed_ptr);

	/* Even with seed=0, the LCG should produce different values
	 * because of the additive constant: seed = seed * 0x19660d + 0x3c6ef35f */
	if (seed_after_first == 0) {
		fprintf(stderr, "FAIL: seed did not change from 0 after first call\n");
		return 1;
	}
	if (first == second) {
		fprintf(stderr, "FAIL: first two calls with seed=0 produced same value\n");
		return 1;
	}

	printf("  PASS: zero-seed RNG still varies\n");
	return 0;
}

static int test_rng2_range(struct macho_dylib_info *info)
{
	/* Test bb_random_Rnd2(lo, hi) produces values in range */
	bb_random_Rnd2_t rng2_func = (bb_random_Rnd2_t)(info->slide + VADDR_BB_RANDOM_RND2);
	uint32_t *seed_ptr = (uint32_t *)(info->slide + VADDR_BB_RANDOM_SEED);

	*seed_ptr = 42;
	printf("  Rnd2 range test (lo=5.0, hi=10.0):\n");

	int in_range = 0;
	int unique = 0;
	float prev = -1;
	for (int i = 0; i < 20; i++) {
		float v = rng2_func(5.0f, 10.0f);
		if (v >= 5.0f && v <= 10.0f)
			in_range++;
		if (v != prev) unique++;
		prev = v;
		if (i < 5) printf("    call %d: %.4f\n", i, v);
	}
	printf("    ... (in_range=%d/20, unique=%d/20)\n", in_range, unique);

	if (in_range < 20) {
		fprintf(stderr, "FAIL: %d/20 values out of range [5,10]\n", 20 - in_range);
		return 1;
	}
	if (unique < 15) {
		fprintf(stderr, "FAIL: only %d/20 unique values in range test\n", unique);
		return 1;
	}

	printf("  PASS: Rnd2 produces varied values in range\n");
	return 0;
}

int main(int argc, char **argv)
{
	const char *dylib_path = "../necrodancer/depot_247086/NecroDancerSP.app/Contents/MacOS/libnecrolevel.dylib";

	if (argc > 1)
		dylib_path = argv[1];

	printf("Loading libnecrolevel.dylib from: %s\n", dylib_path);

	struct macho_dylib_info *info = dylib_loader_load(dylib_path);
	if (!info) {
		fprintf(stderr, "FAIL: could not load libnecrolevel.dylib\n");
		return 1;
	}

	printf("Loaded at slide=0x%lx, %u symbols\n\n", (unsigned long)info->slide, info->nsyms);

	int failures = 0;

	printf("Test 1: RNG variation\n");
	failures += test_rng_variation(info);
	printf("\n");

	printf("Test 2: Zero-seed behavior\n");
	failures += test_rng_zero_seed(info);
	printf("\n");

	printf("Test 3: Rnd2 range\n");
	failures += test_rng2_range(info);
	printf("\n");

	if (failures) {
		printf("FAILED: %d test(s) failed\n", failures);
		return 1;
	}

	printf("ALL PASSED\n");
	return 0;
}
