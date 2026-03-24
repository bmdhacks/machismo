/*
 * Test harness for ARMv8.1 LSE atomic instruction emulation.
 *
 * Generates each LSE instruction variant, patches it with lse_emul_patch,
 * executes the patched island, and verifies results against a reference
 * implementation.
 *
 * Build: gcc -O2 -g -D_GNU_SOURCE -I../src -o test_lse_emul \
 *            test_lse_emul.c ../src/lse_emul.c
 * Run:   ./test_lse_emul
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>
#include "lse_emul.h"

/* ================================================================
 * ARM64 instruction encoding helpers (for test function generation)
 * ================================================================ */

/* RET */
#define ENC_RET 0xD65F03C0
/* NOP */
#define ENC_NOP 0xD503201F

/* STP Xt1, Xt2, [SP, #imm]! (pre-index, signed offset in multiples of 8) */
static uint32_t enc_stp_pre_sp(int rt1, int rt2, int simm7)
{
	uint32_t imm = ((uint32_t)(simm7 / 8)) & 0x7F;
	return 0xA9800000 | (imm << 15) | (rt2 << 10) | (31 << 5) | rt1;
}

/* STP Xt1, Xt2, [SP, #imm] (signed offset, no writeback) */
static uint32_t enc_stp_off_sp(int rt1, int rt2, int simm7)
{
	uint32_t imm = ((uint32_t)(simm7 / 8)) & 0x7F;
	return 0xA9000000 | (imm << 15) | (rt2 << 10) | (31 << 5) | rt1;
}

/* LDP Xt1, Xt2, [SP, #imm] (signed offset, no writeback) */
static uint32_t enc_ldp_off_sp(int rt1, int rt2, int simm7)
{
	uint32_t imm = ((uint32_t)(simm7 / 8)) & 0x7F;
	return 0xA9400000 | (imm << 15) | (rt2 << 10) | (31 << 5) | rt1;
}

/* LDP Xt1, Xt2, [SP], #imm (post-index) */
static uint32_t enc_ldp_post_sp(int rt1, int rt2, int simm7)
{
	uint32_t imm = ((uint32_t)(simm7 / 8)) & 0x7F;
	return 0xA8C00000 | (imm << 15) | (rt2 << 10) | (31 << 5) | rt1;
}

/* MOV Xd, Xn (ORR Xd, XZR, Xn) */
static uint32_t enc_mov_x(int rd, int rn)
{
	return 0xAA000000 | (rn << 16) | (31 << 5) | rd;
}

/* LDR Xt, [Xn, #uimm] (unsigned offset, 8-byte aligned) */
static uint32_t enc_ldr_uoff(int rt, int rn, int uimm)
{
	uint32_t imm12 = (uimm / 8) & 0xFFF;
	return 0xF9400000 | (imm12 << 10) | (rn << 5) | rt;
}

/* STR Xt, [Xn, #uimm] (unsigned offset, 8-byte aligned) */
static uint32_t enc_str_uoff(int rt, int rn, int uimm)
{
	uint32_t imm12 = (uimm / 8) & 0xFFF;
	return 0xF9000000 | (imm12 << 10) | (rn << 5) | rt;
}

/* MOVZ Xd, #imm16 */
static uint32_t enc_movz_x(int rd, uint16_t imm16)
{
	return 0xD2800000 | ((uint32_t)imm16 << 5) | rd;
}

/* ================================================================
 * LSE instruction encoder (inverse of decode_lse in lse_emul.c)
 * ================================================================ */

enum lse_op {
	LSE_LDADD, LSE_LDCLR, LSE_LDEOR, LSE_LDSET,
	LSE_LDSMAX, LSE_LDSMIN, LSE_LDUMAX, LSE_LDUMIN,
	LSE_SWP, LSE_CAS
};

static const char *op_names[] = {
	"LDADD", "LDCLR", "LDEOR", "LDSET",
	"LDSMAX", "LDSMIN", "LDUMAX", "LDUMIN",
	"SWP", "CAS"
};

static const char *size_names[] = {"byte", "half", "word", "dword"};
static const char *order_names[] = {"plain", "release", "acquire", "acq+rel"};

static uint32_t encode_lse(int op, int size, int acquire, int release,
                           int rs, int rt, int rn)
{
	if (op == LSE_CAS) {
		return ((uint32_t)size << 30) | 0x08A07C00
			| ((uint32_t)acquire << 23)
			| ((uint32_t)rs << 16)
			| ((uint32_t)release << 15)
			| ((uint32_t)rn << 5)
			| (uint32_t)rt;
	}
	int o3 = (op == LSE_SWP) ? 1 : 0;
	int opc = (op == LSE_SWP) ? 0 : op;
	return ((uint32_t)size << 30) | 0x38200000
		| ((uint32_t)acquire << 23)
		| ((uint32_t)release << 22)
		| ((uint32_t)rs << 16)
		| ((uint32_t)o3 << 15)
		| ((uint32_t)opc << 12)
		| ((uint32_t)rn << 5)
		| (uint32_t)rt;
}

/* ================================================================
 * Reference implementation
 * ================================================================ */

static uint64_t size_mask(int size)
{
	switch (size) {
	case 0: return 0xFF;
	case 1: return 0xFFFF;
	case 2: return 0xFFFFFFFF;
	case 3: return 0xFFFFFFFFFFFFFFFF;
	}
	return 0;
}

/* Sign-extend from bit width to 64 bits for signed comparisons */
static int64_t sign_ext(uint64_t val, int size)
{
	switch (size) {
	case 0: return (int64_t)(int8_t)(uint8_t)val;
	case 1: return (int64_t)(int16_t)(uint16_t)val;
	case 2: return (int64_t)(int32_t)(uint32_t)val;
	case 3: return (int64_t)val;
	}
	return (int64_t)val;
}

struct lse_result {
	uint64_t mem_after;
	uint64_t rt_after;
	uint64_t rs_after;
};

static struct lse_result compute_expected(int op, int size,
	uint64_t mem_before, uint64_t rs_val, uint64_t rt_val)
{
	struct lse_result r;
	uint64_t mask = size_mask(size);
	uint64_t m = mem_before & mask;
	uint64_t s = rs_val & mask;
	uint64_t t = rt_val & mask;

	r.rt_after = m;  /* most ops return old memory value in Rt */
	r.rs_after = rs_val;  /* Rs unchanged for non-CAS */

	switch (op) {
	case LSE_LDADD:  r.mem_after = (m + s) & mask; break;
	case LSE_LDCLR:  r.mem_after = (m & ~s) & mask; break;
	case LSE_LDEOR:  r.mem_after = (m ^ s) & mask; break;
	case LSE_LDSET:  r.mem_after = (m | s) & mask; break;
	case LSE_LDSMAX:
		r.mem_after = (sign_ext(m, size) >= sign_ext(s, size)) ? m : s;
		r.mem_after &= mask;
		break;
	case LSE_LDSMIN:
		r.mem_after = (sign_ext(m, size) <= sign_ext(s, size)) ? m : s;
		r.mem_after &= mask;
		break;
	case LSE_LDUMAX:
		r.mem_after = (m >= s) ? m : s;
		break;
	case LSE_LDUMIN:
		r.mem_after = (m <= s) ? m : s;
		break;
	case LSE_SWP:
		r.mem_after = s;
		break;
	case LSE_CAS:
		if (m == s) {
			r.mem_after = t;
		} else {
			r.mem_after = m;
		}
		r.rs_after = m;  /* CAS always writes old value to Rs */
		r.rt_after = rt_val;  /* CAS doesn't modify Rt */
		break;
	}
	return r;
}

/* ================================================================
 * Test function generator and executor
 * ================================================================ */

struct test_params {
	uint64_t *addr;     /* +0:  pointer to memory location */
	uint64_t rs_val;    /* +8:  value for Rs register */
	uint64_t rt_val;    /* +16: value for Rt register (CAS new value) */
	uint64_t rt_out;    /* +24: Rt value after operation */
	uint64_t rs_out;    /* +32: Rs value after operation */
};

/*
 * Generate a callable test function in the code page.
 *
 * Standard register assignment: Rs=x20, Rt=x21, Rn=x19
 * The generated function loads params from struct (x0), executes the
 * LSE instruction (which gets patched to B island), stores results back.
 *
 * For edge cases, rs_reg/rt_reg/rn_reg can be overridden.
 *
 * Returns the offset (in words) to the LSE instruction within the function.
 * *fn_size is set to the total function size in words.
 */
static int generate_test_fn(uint32_t *code, int rs_reg, int rt_reg, int rn_reg,
                            uint32_t lse_insn, int *fn_size)
{
	int n = 0;
	int lse_offset;

	/* Save callee-saved registers and struct pointer */
	code[n++] = enc_stp_pre_sp(19, 20, -32);   /* STP x19,x20,[SP,#-32]! */
	code[n++] = enc_stp_off_sp(21, 22, 16);    /* STP x21,x22,[SP,#16] */
	code[n++] = enc_mov_x(22, 0);              /* MOV x22, x0 (struct ptr) */

	/* Load parameters into target registers */
	code[n++] = enc_ldr_uoff(rn_reg, 22, 0);   /* LDR Xrn, [x22, #0] (addr) */
	if (rs_reg != rn_reg)
		code[n++] = enc_ldr_uoff(rs_reg, 22, 8);  /* LDR Xrs, [x22, #8] */
	if (rt_reg != 31 && rt_reg != rn_reg && rt_reg != rs_reg)
		code[n++] = enc_ldr_uoff(rt_reg, 22, 16); /* LDR Xrt, [x22, #16] */

	/* Handle Rs==Rn: Rs gets loaded AFTER Rn, overwriting the address.
	 * For this edge case, the caller must set rs_val == addr. */
	if (rs_reg == rn_reg)
		;  /* Rn already loaded from addr field; Rs IS the address */

	/* Handle Rt==Rn for CAS: load rt_val into Rn, overwriting addr.
	 * Save addr first if needed. This case is unusual. */
	if (rt_reg == rn_reg && rt_reg != 31) {
		/* addr was already loaded into rn_reg, but we need rt_val there.
		 * For CAS Rt==Rn test, skip loading rt_val — use whatever is in Rn. */
	}

	/* The LSE instruction — this gets patched by lse_emul_patch */
	lse_offset = n;
	code[n++] = lse_insn;

	/* NOP — island branches back here */
	code[n++] = ENC_NOP;

	/* Store results */
	if (rt_reg != 31)
		code[n++] = enc_str_uoff(rt_reg, 22, 24);  /* STR Xrt, [x22, #24] */
	else
		;  /* Rt=XZR: old value discarded */

	code[n++] = enc_str_uoff(rs_reg, 22, 32);      /* STR Xrs, [x22, #32] */

	/* Restore and return */
	code[n++] = enc_ldp_off_sp(21, 22, 16);    /* LDP x21,x22,[SP,#16] */
	code[n++] = enc_ldp_post_sp(19, 20, 32);   /* LDP x19,x20,[SP],#32 */
	code[n++] = ENC_RET;

	*fn_size = n;
	return lse_offset;
}

/* ================================================================
 * Test runner
 * ================================================================ */

/* Memory layout: one contiguous RWX region for code + islands */
#define CODE_SIZE  (256 * 1024)
#define POOL_SIZE  (256 * 1024)

static uint32_t *code_base;
static uint32_t *pool_cur;
static uint32_t *pool_end;
static int code_offset;  /* next free word in code_base */

static int tests_run, tests_passed, tests_failed;

static void init_pools(void)
{
	void *base = mmap(NULL, CODE_SIZE + POOL_SIZE,
	                  PROT_READ | PROT_WRITE | PROT_EXEC,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	code_base = (uint32_t *)base;
	pool_cur = (uint32_t *)((char *)base + CODE_SIZE);
	pool_end = (uint32_t *)((char *)base + CODE_SIZE + POOL_SIZE);
	code_offset = 0;
}

static bool run_one_test(int op, int size, int acquire, int release,
                         int rs_reg, int rt_reg, int rn_reg,
                         uint64_t mem_val, uint64_t rs_val, uint64_t rt_val,
                         const char *label)
{
	tests_run++;
	uint64_t mask = size_mask(size);

	/* Encode the LSE instruction */
	uint32_t insn = encode_lse(op, size, acquire, release, rs_reg, rt_reg, rn_reg);

	/* Generate the test function */
	uint32_t *fn_code = &code_base[code_offset];
	int fn_size;
	int lse_off = generate_test_fn(fn_code, rs_reg, rt_reg, rn_reg, insn, &fn_size);

	/* Patch the LSE instruction */
	uint32_t *lse_ptr = &fn_code[lse_off];
	int patched = lse_emul_patch(lse_ptr, 4, &pool_cur, pool_end);
	if (patched != 1) {
		fprintf(stderr, "FAIL: %s — lse_emul_patch returned %d (expected 1)\n",
		        label, patched);
		tests_failed++;
		code_offset += fn_size;
		return false;
	}

	/* Flush icache */
	__builtin___clear_cache((char *)fn_code, (char *)(fn_code + fn_size));
	__builtin___clear_cache((char *)pool_cur - POOL_SIZE, (char *)pool_cur);

	code_offset += fn_size;

	/* Set up memory and params */
	uint64_t mem __attribute__((aligned(16))) = mem_val & mask;
	struct test_params params = {
		.addr = &mem,
		.rs_val = rs_val & mask,
		.rt_val = rt_val & mask,
		.rt_out = 0xDEADDEADDEADDEAD,
		.rs_out = 0xDEADDEADDEADDEAD,
	};

	/* For Rs==Rn edge case, the address IS the rs_val since they share a reg */
	if (rs_reg == rn_reg) {
		params.rs_val = (uint64_t)&mem;
		/* The actual operation will use the address as the Rs operand */
		rs_val = (uint64_t)&mem;
	}

	/* Call the generated function */
	typedef void (*test_fn_t)(struct test_params *);
	test_fn_t fn = (test_fn_t)fn_code;
	fn(&params);

	/* Compute expected results */
	struct lse_result expected;
	if (rs_reg == rn_reg) {
		/* Rs==Rn: the operation uses the address pointer as the operand */
		expected = compute_expected(op, size, mem_val, (uint64_t)&mem, rt_val);
	} else {
		expected = compute_expected(op, size, mem_val, rs_val, rt_val);
	}

	/* Verify results */
	bool ok = true;
	uint64_t mem_result = mem & mask;
	uint64_t expected_mem = expected.mem_after & mask;

	if (mem_result != expected_mem) {
		fprintf(stderr, "FAIL: %s — mem: got 0x%llx, expected 0x%llx\n",
		        label, (unsigned long long)mem_result,
		        (unsigned long long)expected_mem);
		ok = false;
	}

	if (rt_reg != 31 && op != LSE_CAS) {
		uint64_t rt_result = params.rt_out & mask;
		uint64_t expected_rt = expected.rt_after & mask;
		if (rt_result != expected_rt) {
			fprintf(stderr, "FAIL: %s — Rt: got 0x%llx, expected 0x%llx\n",
			        label, (unsigned long long)rt_result,
			        (unsigned long long)expected_rt);
			ok = false;
		}
	}

	if (op == LSE_CAS) {
		uint64_t rs_result = params.rs_out & mask;
		uint64_t expected_rs = expected.rs_after & mask;
		if (rs_result != expected_rs) {
			fprintf(stderr, "FAIL: %s — Rs(CAS): got 0x%llx, expected 0x%llx\n",
			        label, (unsigned long long)rs_result,
			        (unsigned long long)expected_rs);
			ok = false;
		}
	}

	if (ok) {
		tests_passed++;
	} else {
		tests_failed++;
		fprintf(stderr, "  insn=0x%08x mem_before=0x%llx rs=0x%llx rt=0x%llx\n",
		        insn, (unsigned long long)(mem_val & mask),
		        (unsigned long long)(rs_val & mask),
		        (unsigned long long)(rt_val & mask));
	}

	return ok;
}

/* ================================================================
 * Test matrix
 * ================================================================ */

struct test_values {
	uint64_t mem;
	uint64_t rs;
	uint64_t rt;
};

/* Standard register assignment: Rs=x20, Rt=x21, Rn=x19 */
#define STD_RS 20
#define STD_RT 21
#define STD_RN 19

static void test_all_basic(void)
{
	printf("=== Basic tests: 10 ops × 4 orderings × 4 sizes ===\n");

	struct test_values vals[] = {
		{ 0x0A0B0C0D0E0F1011ULL, 0x0102030405060708ULL, 0x1122334455667788ULL },
		{ 0, 1, 0 },
		{ 0xFF, 0x01, 0 },
		{ 0x7FFFFFFFFFFFFFFFULL, 1, 0 },
		/* Negative values for signed min/max */
		{ 0xFFFFFFFFFFFFFFF0ULL, 0x0000000000000010ULL, 0 },  /* -16, +16 */
		{ 0x80, 0x7F, 0 },  /* -128 vs +127 in byte */
	};
	int nvals = sizeof(vals) / sizeof(vals[0]);

	for (int op = LSE_LDADD; op <= LSE_CAS; op++) {
		for (int ord = 0; ord < 4; ord++) {
			int acquire = (ord >> 1) & 1;
			int release = ord & 1;
			for (int sz = 0; sz <= 3; sz++) {
				/* Use first value set for basic pass */
				struct test_values *v = &vals[0];

				/* For CAS, also test match and mismatch */
				if (op == LSE_CAS) {
					char label[128];
					/* CAS match: rs_val == mem (should swap) */
					snprintf(label, sizeof(label), "CAS %s %s match",
					         size_names[sz], order_names[ord]);
					run_one_test(op, sz, acquire, release,
					             STD_RS, STD_RT, STD_RN,
					             0x42, 0x42, 0x99, label);
					/* CAS mismatch: rs_val != mem (should NOT swap) */
					snprintf(label, sizeof(label), "CAS %s %s mismatch",
					         size_names[sz], order_names[ord]);
					run_one_test(op, sz, acquire, release,
					             STD_RS, STD_RT, STD_RN,
					             0x42, 0x43, 0x99, label);
					continue;
				}

				char label[128];
				snprintf(label, sizeof(label), "%s %s %s",
				         op_names[op], size_names[sz], order_names[ord]);
				run_one_test(op, sz, acquire, release,
				             STD_RS, STD_RT, STD_RN,
				             v->mem, v->rs, v->rt, label);
			}
		}
	}

	/* Extra value-coverage tests for signed/unsigned min/max */
	printf("=== Signed/unsigned min/max value coverage ===\n");
	for (int sz = 0; sz <= 3; sz++) {
		for (int vi = 0; vi < nvals; vi++) {
			struct test_values *v = &vals[vi];
			char label[128];
			for (int op = LSE_LDSMAX; op <= LSE_LDUMIN; op++) {
				snprintf(label, sizeof(label), "%s %s val%d",
				         op_names[op], size_names[sz], vi);
				run_one_test(op, sz, 1, 1, STD_RS, STD_RT, STD_RN,
				             v->mem, v->rs, v->rt, label);
			}
		}
	}
}

static void test_edge_cases(void)
{
	printf("=== Edge case: Rt == Rn (address clobbered by load) ===\n");
	for (int op = LSE_LDADD; op <= LSE_SWP; op++) {
		char label[128];
		snprintf(label, sizeof(label), "%s dword Rt==Rn", op_names[op]);
		/* Rs=x20, Rt=x19, Rn=x19 — same register for address and result */
		run_one_test(op, 3, 1, 1, 20, 19, 19,
		             0xAAAABBBBCCCCDDDD, 0x0000000000000001, 0, label);
	}

	printf("=== Edge case: Rt == XZR (STADD etc., discard old value) ===\n");
	for (int op = LSE_LDADD; op <= LSE_SWP; op++) {
		char label[128];
		snprintf(label, sizeof(label), "%s dword Rt==XZR", op_names[op]);
		run_one_test(op, 3, 0, 0, STD_RS, 31, STD_RN,
		             0x1000, 0x0001, 0, label);
	}

	printf("=== Edge case: CAS match/mismatch with various sizes ===\n");
	for (int sz = 0; sz <= 3; sz++) {
		char label[128];
		snprintf(label, sizeof(label), "CAS %s edge match", size_names[sz]);
		run_one_test(LSE_CAS, sz, 1, 1, STD_RS, STD_RT, STD_RN,
		             0xFF, 0xFF, 0xAA, label);
		snprintf(label, sizeof(label), "CAS %s edge mismatch", size_names[sz]);
		run_one_test(LSE_CAS, sz, 1, 1, STD_RS, STD_RT, STD_RN,
		             0xFF, 0xFE, 0xAA, label);
	}

	printf("=== Edge case: byte/half sizes ===\n");
	/* Byte: verify no overflow into upper bits */
	run_one_test(LSE_LDADD, 0, 1, 1, STD_RS, STD_RT, STD_RN,
	             0xFF, 0x01, 0, "LDADD byte overflow");
	/* Half: verify 16-bit boundary */
	run_one_test(LSE_LDADD, 1, 1, 1, STD_RS, STD_RT, STD_RN,
	             0xFFFF, 0x0001, 0, "LDADD half overflow");
	/* Word: verify 32-bit boundary */
	run_one_test(LSE_LDADD, 2, 1, 1, STD_RS, STD_RT, STD_RN,
	             0xFFFFFFFF, 0x00000001, 0, "LDADD word overflow");
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void)
{
	printf("LSE atomic emulation test harness\n\n");

	init_pools();

	test_all_basic();
	test_edge_cases();

	printf("\n=== Results: %d/%d passed, %d failed ===\n",
	       tests_passed, tests_run, tests_failed);

	if (tests_failed > 0) {
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
