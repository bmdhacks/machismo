/*
 * Standalone aarch64 commpage emulation for Mach-O loader.
 *
 * On macOS arm64, the commpage lives at 0x0000000FFFFFC000 (4 pages = 16KB).
 * We map it and populate basic fields that macOS binaries may read.
 *
 * Reference: XNU osfmk/arm/commpage/commpage.c
 */

#include "commpage.h"
#include <sys/mman.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/sysinfo.h>

#ifdef __aarch64__
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif

/*
 * arm64 commpage layout constants (from XNU).
 * The commpage base on arm64 is at a high user-space address.
 */
#define ARM64_COMMPAGE_BASE    0x0000000FFFFFC000UL
#define ARM64_COMMPAGE_LENGTH  0x4000  /* 4 pages = 16KB */

/*
 * Commpage field offsets (relative to base).
 * These match XNU's commpage layout for arm64.
 */
#define CP_SIGNATURE       0x000  /* char[16] */
#define CP_VERSION         0x01E  /* uint16_t */
#define CP_NCPUS           0x022  /* uint8_t */
#define CP_ACTIVE_CPUS     0x034  /* uint8_t */
#define CP_PHYSICAL_CPUS   0x035  /* uint8_t */
#define CP_LOGICAL_CPUS    0x036  /* uint8_t */
#define CP_USER_PAGE_SHIFT 0x02C  /* uint8_t (arm64 uses unified offset) */
#define CP_KERNEL_PAGE_SHIFT 0x02D /* uint8_t */
#define CP_MEMORY_SIZE     0x038  /* uint64_t */
#define CP_CPU_CAPS64      0x010  /* uint64_t */
#define CP_CPU_CAPS        0x020  /* uint32_t */

/*
 * arm64 CPU capability flags (matching XNU commpage caps).
 * These are what macOS arm64 code checks via commpage.
 */
#define kHasNEON          0x00000001
#define kHasNEON_HPF      0x00000002  /* half-precision float */
#define kHasNEON_FP16     0x00000004
#define kHasAES           0x00000008  /* AES crypto instructions */
#define kHasSHA1          0x00000010
#define kHasSHA256        0x00000020
#define kHasCRC32         0x00000040
#define kHasLSE           0x00000080  /* Large System Extensions (atomics) */
#define kHasFEAT_FP16     0x00000100
#define kHasFEAT_DotProd  0x00000200
#define kHasFEAT_SHA3     0x00000400
#define kHasFEAT_SHA512   0x00000800

static const char* SIGNATURE64 = "commpage arm64";

void commpage_setup(bool _64bit)
{
	(void)_64bit; /* arm64 only uses 64-bit commpage */
	uint8_t* commpage;
	struct sysinfo si;

	commpage = (uint8_t*) mmap((void*)ARM64_COMMPAGE_BASE,
			ARM64_COMMPAGE_LENGTH,
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			-1, 0);

	if (commpage == MAP_FAILED)
	{
		/* Try without MAP_FIXED_NOREPLACE for older kernels */
		commpage = (uint8_t*) mmap((void*)ARM64_COMMPAGE_BASE,
				ARM64_COMMPAGE_LENGTH,
				PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
				-1, 0);

		if (commpage == MAP_FAILED)
		{
			fprintf(stderr, "Cannot mmap commpage at %p: %s\n",
					(void*)ARM64_COMMPAGE_BASE, strerror(errno));
			fprintf(stderr, "Continuing without commpage (some features may not work)\n");
			return;
		}
	}

	memset(commpage, 0, ARM64_COMMPAGE_LENGTH);

	/* Signature */
	strncpy((char*)(commpage + CP_SIGNATURE), SIGNATURE64, 16);

	/* Version */
	*(uint16_t*)(commpage + CP_VERSION) = 14; /* match recent XNU */

	/* CPU count */
	uint8_t ncpus = sysconf(_SC_NPROCESSORS_CONF);
	uint8_t nactive = sysconf(_SC_NPROCESSORS_ONLN);
	*(uint8_t*)(commpage + CP_NCPUS) = ncpus;
	*(uint8_t*)(commpage + CP_ACTIVE_CPUS) = nactive;
	*(uint8_t*)(commpage + CP_PHYSICAL_CPUS) = ncpus;
	*(uint8_t*)(commpage + CP_LOGICAL_CPUS) = ncpus;

	/* Page size */
	uint8_t page_shift = (uint8_t)__builtin_ctzl(sysconf(_SC_PAGESIZE));
	*(uint8_t*)(commpage + CP_USER_PAGE_SHIFT) = page_shift;
	*(uint8_t*)(commpage + CP_KERNEL_PAGE_SHIFT) = page_shift;

	/* Memory size */
	if (sysinfo(&si) == 0)
	{
		*(uint64_t*)(commpage + CP_MEMORY_SIZE) = (uint64_t)si.totalram * si.mem_unit;
	}

	/* CPU capabilities from Linux hwcaps */
	uint64_t caps = 0;

#ifdef __aarch64__
	unsigned long hwcap = getauxval(AT_HWCAP);
	unsigned long hwcap2 = getauxval(AT_HWCAP2);

	/* NEON is mandatory on aarch64 */
	caps |= kHasNEON;

	if (hwcap & HWCAP_FPHP)
		caps |= kHasNEON_HPF | kHasNEON_FP16 | kHasFEAT_FP16;
	if (hwcap & HWCAP_AES)
		caps |= kHasAES;
	if (hwcap & HWCAP_SHA1)
		caps |= kHasSHA1;
	if (hwcap & HWCAP_SHA2)
		caps |= kHasSHA256;
	if (hwcap & HWCAP_CRC32)
		caps |= kHasCRC32;
	if (hwcap & HWCAP_ATOMICS)
		caps |= kHasLSE;
	if (hwcap & HWCAP_ASIMDDP)
		caps |= kHasFEAT_DotProd;

	/* hwcap2 features (Linux 5.x+) */
	(void)hwcap2;
#if defined(HWCAP2_SHA3)
	if (hwcap2 & HWCAP2_SHA3)
		caps |= kHasFEAT_SHA3;
#endif
#if defined(HWCAP2_SHA512)
	if (hwcap2 & HWCAP2_SHA512)
		caps |= kHasFEAT_SHA512;
#endif

#endif /* __aarch64__ */

	*(uint64_t*)(commpage + CP_CPU_CAPS64) = caps;
	*(uint32_t*)(commpage + CP_CPU_CAPS) = (uint32_t)caps;

	/* Make commpage read-only */
	mprotect(commpage, ARM64_COMMPAGE_LENGTH, PROT_READ);
}

unsigned long commpage_address(bool _64bit)
{
	(void)_64bit;
	return ARM64_COMMPAGE_BASE;
}
