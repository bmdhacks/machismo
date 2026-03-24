/*
 * ARMv8.1 LSE atomic instruction emulation via branch islands.
 *
 * Replaces each LSE instruction with B to an island containing
 * an equivalent LDXR/STXR (load-linked/store-conditional) loop.
 *
 * Supported instruction families:
 *   LDADD, LDCLR, LDEOR, LDSET (and ST variants when Rt=XZR)
 *   LDSMAX, LDSMIN, LDUMAX, LDUMIN
 *   SWP
 *   CAS
 */

#include "lse_emul.h"
#include <stdio.h>

/* ARM64 instruction encoding helpers */

/* B (unconditional branch): offset in words, ±128MB */
static inline uint32_t enc_b(int32_t word_offset)
{
	return 0x14000000 | (word_offset & 0x03FFFFFF);
}

/* CBNZ Wt, offset (offset in words from this instruction) */
static inline uint32_t enc_cbnz_w(int rt, int32_t word_offset)
{
	return 0x35000000 | ((word_offset & 0x7FFFF) << 5) | rt;
}

/* STR Xt, [SP, #-16]! (pre-index, save to stack) */
static inline uint32_t enc_str_pre(int rt)
{
	return 0xF81F0FE0 | rt;  /* str Xt, [sp, #-16]! */
}

/* LDR Xt, [SP], #16 (post-index, restore from stack) */
static inline uint32_t enc_ldr_post(int rt)
{
	return 0xF84107E0 | rt;  /* ldr Xt, [sp], #16 */
}

/* LDXR / LDAXR — load exclusive (32 or 64 bit)
 * size: 2=32bit, 3=64bit
 * acquire: 1=LDAXR, 0=LDXR */
static inline uint32_t enc_ldxr(int size, int acquire, int rt, int rn)
{
	uint32_t base = 0x08407C00;  /* LDXR */
	if (acquire) base |= (1 << 15);  /* LDAXR */
	return base | ((uint32_t)size << 30) | (rn << 5) | rt;
}

/* LDXRB / LDAXRB — load exclusive byte */
static inline uint32_t enc_ldxrb(int acquire, int rt, int rn)
{
	return enc_ldxr(0, acquire, rt, rn);
}

/* LDXRH / LDAXRH — load exclusive halfword */
static inline uint32_t enc_ldxrh(int acquire, int rt, int rn)
{
	return enc_ldxr(1, acquire, rt, rn);
}

/* STXR / STLXR — store exclusive
 * size: 0=byte, 1=half, 2=word, 3=dword
 * release: 1=STLXR, 0=STXR
 * rs: status register (W-size) */
static inline uint32_t enc_stxr(int size, int release, int rs, int rt, int rn)
{
	uint32_t base = 0x08007C00;  /* STXR */
	if (release) base |= (1 << 15);  /* STLXR */
	return base | ((uint32_t)size << 30) | (rs << 16) | (rn << 5) | rt;
}

/* ADD Xd, Xn, Xm (64-bit) or ADD Wd, Wn, Wm (32-bit) */
static inline uint32_t enc_add(int is64, int rd, int rn, int rm)
{
	return (is64 ? 0x8B000000u : 0x0B000000u) | (rm << 16) | (rn << 5) | rd;
}

/* BIC Xd, Xn, Xm (AND-NOT) */
static inline uint32_t enc_bic(int is64, int rd, int rn, int rm)
{
	return (is64 ? 0x8A200000u : 0x0A200000u) | (rm << 16) | (rn << 5) | rd;
}

/* EOR Xd, Xn, Xm */
static inline uint32_t enc_eor(int is64, int rd, int rn, int rm)
{
	return (is64 ? 0xCA000000u : 0x4A000000u) | (rm << 16) | (rn << 5) | rd;
}

/* ORR Xd, Xn, Xm */
static inline uint32_t enc_orr(int is64, int rd, int rn, int rm)
{
	return (is64 ? 0xAA000000u : 0x2A000000u) | (rm << 16) | (rn << 5) | rd;
}

/* CMP Xn, Xm (SUBS XZR, Xn, Xm) */
static inline uint32_t enc_cmp(int is64, int rn, int rm)
{
	return (is64 ? 0xEB000000u : 0x6B000000u) | (rm << 16) | (rn << 5) | 31;
}

/* CSEL Xd, Xn, Xm, cond */
static inline uint32_t enc_csel(int is64, int rd, int rn, int rm, int cond)
{
	return (is64 ? 0x9A800000u : 0x1A800000u) | (rm << 16) | (cond << 12) | (rn << 5) | rd;
}

/* MOV Xd, Xn (ORR Xd, XZR, Xn) */
static inline uint32_t enc_mov(int is64, int rd, int rn)
{
	return enc_orr(is64, rd, 31, rn);
}

/* Condition codes for CSEL */
#define COND_GT 0xC  /* signed greater than */
#define COND_LT 0xB  /* signed less than */
#define COND_HI 0x8  /* unsigned higher */
#define COND_LO 0x3  /* unsigned lower (carry clear) */
#define COND_EQ 0x0  /* equal */
#define COND_NE 0x1  /* not equal */

/* Pick a scratch register that doesn't conflict with rs, rt, rn.
 * Avoids SP(31) and the three operand registers. */
static int pick_scratch(int rs, int rt, int rn)
{
	for (int r = 16; r >= 9; r--) {
		if (r != rs && r != rt && r != rn)
			return r;
	}
	/* Fallback to higher registers */
	for (int r = 17; r <= 28; r++) {
		if (r != rs && r != rt && r != rn)
			return r;
	}
	return 15;  /* should never get here */
}

/* LSE instruction classification */
enum lse_op {
	LSE_LDADD, LSE_LDCLR, LSE_LDEOR, LSE_LDSET,
	LSE_LDSMAX, LSE_LDSMIN, LSE_LDUMAX, LSE_LDUMIN,
	LSE_SWP, LSE_CAS, LSE_NONE
};

struct lse_decoded {
	enum lse_op op;
	int size;      /* 0=byte, 1=half, 2=word, 3=dword */
	int acquire;   /* A bit */
	int release;   /* L/R bit */
	int rs;        /* source/status register */
	int rt;        /* destination register */
	int rn;        /* base address register */
};

static int decode_lse(uint32_t instr, struct lse_decoded *d)
{
	/* LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP:
	 * size[31:30] 111000 A[23] R[22] 1 Rs[20:16] o3[15] opc[14:12] 00 Rn[9:5] Rt[4:0] */
	if ((instr & 0x3F200C00) == 0x38200000) {
		d->size = (instr >> 30) & 3;
		d->acquire = (instr >> 23) & 1;
		d->release = (instr >> 22) & 1;
		d->rs = (instr >> 16) & 0x1F;
		d->rn = (instr >> 5) & 0x1F;
		d->rt = instr & 0x1F;
		int o3 = (instr >> 15) & 1;
		int opc = (instr >> 12) & 7;
		if (o3 == 1) {
			d->op = LSE_SWP;
		} else {
			switch (opc) {
			case 0: d->op = LSE_LDADD; break;
			case 1: d->op = LSE_LDCLR; break;
			case 2: d->op = LSE_LDEOR; break;
			case 3: d->op = LSE_LDSET; break;
			case 4: d->op = LSE_LDSMAX; break;
			case 5: d->op = LSE_LDSMIN; break;
			case 6: d->op = LSE_LDUMAX; break;
			case 7: d->op = LSE_LDUMIN; break;
			}
		}
		return 1;
	}

	/* CAS/CASA/CASL/CASAL:
	 * size[31:30] 001000 1 A[23] 1 Rs[20:16] o0[15] 11111 Rn[9:5] Rt[4:0] */
	if ((instr & 0x3FA07C00) == 0x08A07C00) {
		d->size = (instr >> 30) & 3;
		d->acquire = (instr >> 23) & 1;
		d->release = (instr >> 15) & 1;  /* o0 bit for CAS */
		d->rs = (instr >> 16) & 0x1F;
		d->rn = (instr >> 5) & 0x1F;
		d->rt = instr & 0x1F;
		d->op = LSE_CAS;
		return 1;
	}

	d->op = LSE_NONE;
	return 0;
}

/* Generate island code for an LSE instruction.
 * Returns number of uint32_t words written to island. */
static int emit_island(struct lse_decoded *d, uint32_t *island,
                       uint32_t *orig_addr)
{
	int n = 0;
	/* For ALU ops: size 3 (dword) uses X registers, else W */
	int is_x = (d->size == 3);

	int tmp = pick_scratch(d->rs, d->rt, d->rn);
	int rt_is_zr = (d->rt == 31);

	/* Use Rt directly for the loaded value if Rt != XZR.
	 * If Rt == XZR (STADD etc.), use tmp for the loaded value. */
	int load_reg = rt_is_zr ? tmp : d->rt;

	/* Save scratch register */
	island[n++] = enc_str_pre(tmp);

	if (d->op == LSE_CAS) {
		/* CAS Rs, Rt, [Rn]:
		 *   Load [Rn], compare with Rs.
		 *   If equal, store Rt to [Rn].
		 *   Rs gets the old value from [Rn] regardless.
		 *
		 * We need a second scratch for the loaded value since
		 * we compare against Rs (which we can't clobber until the end). */
		int tmp2 = pick_scratch(d->rs, d->rt, tmp);
		/* Save tmp2 as well — use 16-byte aligned pair */
		island[n++] = enc_str_pre(tmp2);

		/* 1: ldxr/ldaxr tmp2, [Rn] */
		int loop_start = n;
		island[n++] = enc_ldxr(d->size, d->acquire, tmp2, d->rn);
		/* cmp tmp2, Rs */
		island[n++] = enc_cmp(is_x, tmp2, d->rs);
		/* b.ne fail (skip stxr + cbnz = 2 instructions = +3 words forward) */
		island[n++] = 0x54000000 | (3 << 5) | COND_NE;  /* b.ne +3 */
		/* stxr/stlxr w_tmp, Rt, [Rn] */
		island[n++] = enc_stxr(d->size, d->release, tmp, d->rt, d->rn);
		/* cbnz w_tmp, loop_start */
		{ int off = loop_start - (n + 1); island[n] = enc_cbnz_w(tmp, off); n++; }
		/* b done (+1 to skip the clrex) */
		island[n++] = enc_b(2);  /* skip clrex + mov below... actually let me restructure */

		/* Hmm, need to handle the failed CAS path. Let me restructure:
		 * 1: ldaxr  tmp2, [Rn]
		 *    cmp    tmp2, Rs
		 *    b.ne   3f
		 *    stlxr  w_tmp, Rt, [Rn]
		 *    cbnz   w_tmp, 1b
		 * 3: mov    Rs, tmp2          (Rs gets old value)
		 *    clrex                     (clear exclusive on failure path)
		 *    ldr    tmp2, [sp], #16
		 *    ldr    tmp, [sp], #16
		 *    b      return
		 */
		/* Oops, I already emitted some instructions. Let me redo. */
		n = 0;
		island[n++] = enc_str_pre(tmp);
		island[n++] = enc_str_pre(tmp2);

		loop_start = n;
		island[n++] = enc_ldxr(d->size, d->acquire, tmp2, d->rn);
		island[n++] = enc_cmp(is_x, tmp2, d->rs);
		/* b.ne to fail path: skip stlxr(1) + cbnz(1) = +2 instructions = 3 words ahead */
		island[n++] = 0x54000000 | (3 << 5) | COND_NE;
		island[n++] = enc_stxr(d->size, d->release, tmp, d->rt, d->rn);
		{ int off = loop_start - (n + 1); island[n] = enc_cbnz_w(tmp, off); n++; }
		/* Success: fall through. Fail: lands here too. */
		/* Both paths: Rs = loaded value, clear exclusive monitor */
		island[n++] = 0xD5033F5F;  /* clrex */
		if (d->rs != 31)
			island[n++] = enc_mov(is_x, d->rs, tmp2);
		island[n++] = enc_ldr_post(tmp2);
		island[n++] = enc_ldr_post(tmp);
		{ int32_t off = (int32_t)(orig_addr + 1 - &island[n]); island[n] = enc_b(off); n++; }
		return n;
	}

	/* Non-CAS: LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP */

	/* 1: ldxr/ldaxr load_reg, [Rn] */
	int loop_start = n;
	island[n++] = enc_ldxr(d->size, d->acquire, load_reg, d->rn);

	/* Compute new value into tmp */
	switch (d->op) {
	case LSE_LDADD:
		island[n++] = enc_add(is_x, tmp, load_reg, d->rs);
		break;
	case LSE_LDCLR:
		island[n++] = enc_bic(is_x, tmp, load_reg, d->rs);
		break;
	case LSE_LDEOR:
		island[n++] = enc_eor(is_x, tmp, load_reg, d->rs);
		break;
	case LSE_LDSET:
		island[n++] = enc_orr(is_x, tmp, load_reg, d->rs);
		break;
	case LSE_LDSMAX:
		island[n++] = enc_cmp(is_x, load_reg, d->rs);
		island[n++] = enc_csel(is_x, tmp, d->rs, load_reg, COND_LT);
		break;
	case LSE_LDSMIN:
		island[n++] = enc_cmp(is_x, load_reg, d->rs);
		island[n++] = enc_csel(is_x, tmp, d->rs, load_reg, COND_GT);
		break;
	case LSE_LDUMAX:
		island[n++] = enc_cmp(is_x, load_reg, d->rs);
		island[n++] = enc_csel(is_x, tmp, d->rs, load_reg, COND_LO);
		break;
	case LSE_LDUMIN:
		island[n++] = enc_cmp(is_x, load_reg, d->rs);
		island[n++] = enc_csel(is_x, tmp, d->rs, load_reg, COND_HI);
		break;
	case LSE_SWP:
		/* New value is just Rs; tmp holds it for stxr */
		island[n++] = enc_mov(is_x, tmp, d->rs);
		break;
	default:
		break;
	}

	/* stxr/stlxr w_scratch2, tmp, [Rn]
	 * We need a different register for stxr status. Reuse load_reg
	 * if it's not Rn (stxr status reg must differ from Rn and value reg).
	 * For Rt==XZR case, load_reg==tmp, so we need another approach. */
	int status_reg;
	if (rt_is_zr) {
		/* load_reg == tmp, so we need a different status reg.
		 * Pick one that's not tmp, rs, rn. */
		int tmp2 = pick_scratch(tmp, d->rs, d->rn);
		status_reg = tmp2;
		/* We need to save/restore tmp2 as well. Restructure with double save. */
		/* Restart generation with double save */
		n = 0;
		island[n++] = enc_str_pre(tmp);
		island[n++] = enc_str_pre(status_reg);

		load_reg = tmp;
		loop_start = n;
		island[n++] = enc_ldxr(d->size, d->acquire, load_reg, d->rn);

		switch (d->op) {
		case LSE_LDADD:
			island[n++] = enc_add(is_x, status_reg, load_reg, d->rs);
			break;
		case LSE_LDCLR:
			island[n++] = enc_bic(is_x, status_reg, load_reg, d->rs);
			break;
		case LSE_LDEOR:
			island[n++] = enc_eor(is_x, status_reg, load_reg, d->rs);
			break;
		case LSE_LDSET:
			island[n++] = enc_orr(is_x, status_reg, load_reg, d->rs);
			break;
		case LSE_LDSMAX:
			island[n++] = enc_cmp(is_x, load_reg, d->rs);
			island[n++] = enc_csel(is_x, status_reg, d->rs, load_reg, COND_LT);
			break;
		case LSE_LDSMIN:
			island[n++] = enc_cmp(is_x, load_reg, d->rs);
			island[n++] = enc_csel(is_x, status_reg, d->rs, load_reg, COND_GT);
			break;
		case LSE_LDUMAX:
			island[n++] = enc_cmp(is_x, load_reg, d->rs);
			island[n++] = enc_csel(is_x, status_reg, d->rs, load_reg, COND_LO);
			break;
		case LSE_LDUMIN:
			island[n++] = enc_cmp(is_x, load_reg, d->rs);
			island[n++] = enc_csel(is_x, status_reg, d->rs, load_reg, COND_HI);
			break;
		case LSE_SWP:
			island[n++] = enc_mov(is_x, status_reg, d->rs);
			break;
		default:
			break;
		}

		/* stxr uses tmp as status, status_reg as value */
		island[n++] = enc_stxr(d->size, d->release, tmp, status_reg, d->rn);
		{ int off = loop_start - (n + 1); island[n] = enc_cbnz_w(tmp, off); n++; }
		island[n++] = enc_ldr_post(status_reg);
		island[n++] = enc_ldr_post(tmp);
		{ int32_t off = (int32_t)(orig_addr + 1 - &island[n]); island[n] = enc_b(off); n++; }
		return n;
	}

	/* Normal case: Rt != XZR, load_reg == Rt.
	 *
	 * We need two scratch registers:
	 *   tmp:    holds the computed new value for stxr
	 *   status: holds the stxr success/fail result
	 *
	 * When Rt == Rn, ldxr clobbers the address register with the loaded
	 * value. We need a third scratch to preserve the address. */
	int status = pick_scratch(tmp, d->rt, d->rn);

	/* addr_reg: register holding the address for ldxr/stxr.
	 * Normally Rn, but if Rt==Rn we copy Rn to a scratch first. */
	int addr_reg = d->rn;
	int addr_scratch = -1;
	if (d->rt == d->rn && !rt_is_zr) {
		/* Need a third scratch for the address */
		addr_scratch = pick_scratch(tmp, status, d->rs);
		addr_reg = addr_scratch;
	}

	/* Restart with saves */
	n = 0;
	island[n++] = enc_str_pre(tmp);
	island[n++] = enc_str_pre(status);
	if (addr_scratch >= 0) {
		island[n++] = enc_str_pre(addr_scratch);
		island[n++] = enc_mov(1, addr_scratch, d->rn);  /* copy address */
	}

	loop_start = n;
	island[n++] = enc_ldxr(d->size, d->acquire, load_reg, addr_reg);

	switch (d->op) {
	case LSE_LDADD:
		island[n++] = enc_add(is_x, tmp, load_reg, d->rs);
		break;
	case LSE_LDCLR:
		island[n++] = enc_bic(is_x, tmp, load_reg, d->rs);
		break;
	case LSE_LDEOR:
		island[n++] = enc_eor(is_x, tmp, load_reg, d->rs);
		break;
	case LSE_LDSET:
		island[n++] = enc_orr(is_x, tmp, load_reg, d->rs);
		break;
	case LSE_LDSMAX:
		island[n++] = enc_cmp(is_x, load_reg, d->rs);
		island[n++] = enc_csel(is_x, tmp, d->rs, load_reg, COND_LT);
		break;
	case LSE_LDSMIN:
		island[n++] = enc_cmp(is_x, load_reg, d->rs);
		island[n++] = enc_csel(is_x, tmp, d->rs, load_reg, COND_GT);
		break;
	case LSE_LDUMAX:
		island[n++] = enc_cmp(is_x, load_reg, d->rs);
		island[n++] = enc_csel(is_x, tmp, d->rs, load_reg, COND_LO);
		break;
	case LSE_LDUMIN:
		island[n++] = enc_cmp(is_x, load_reg, d->rs);
		island[n++] = enc_csel(is_x, tmp, d->rs, load_reg, COND_HI);
		break;
	case LSE_SWP:
		island[n++] = enc_mov(is_x, tmp, d->rs);
		break;
	default:
		break;
	}

	island[n++] = enc_stxr(d->size, d->release, status, tmp, addr_reg);
	{ int off = loop_start - (n + 1); island[n] = enc_cbnz_w(status, off); n++; }
	if (addr_scratch >= 0)
		island[n++] = enc_ldr_post(addr_scratch);
	island[n++] = enc_ldr_post(status);
	island[n++] = enc_ldr_post(tmp);
	{ int32_t off = (int32_t)(orig_addr + 1 - &island[n]); island[n] = enc_b(off); n++; }
	return n;
}

int lse_emul_patch(uint32_t *code, size_t code_size,
                   uint32_t **pool_ptr, uint32_t *pool_end)
{
	size_t count = code_size / 4;
	uint32_t *island = *pool_ptr;
	int patched = 0;

	for (size_t i = 0; i < count; i++) {
		struct lse_decoded d;
		if (!decode_lse(code[i], &d))
			continue;

		/* Check pool space (max island ~48 bytes = 12 words) */
		if (island + 16 > pool_end) {
			fprintf(stderr, "lse_emul: island pool exhausted after %d patches\n", patched);
			break;
		}

		/* Check branch range: B has ±128MB range */
		int64_t offset = (int64_t)(island - &code[i]);
		if (offset > 0x01FFFFFF || offset < -0x02000000) {
			fprintf(stderr, "lse_emul: island out of B range at %p\n", (void*)&code[i]);
			continue;
		}

		int words = emit_island(&d, island, &code[i]);
		if (words <= 0)
			continue;

		/* Replace original instruction with B to island */
		code[i] = enc_b((int32_t)(island - &code[i]));

		island += words;
		patched++;
	}

	*pool_ptr = island;
	return patched;
}
