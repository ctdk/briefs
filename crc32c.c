/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/*
 * CRC32C-family checksum used for journal record checksums, btree node
 * checksums, and xattr block checksums.
 *
 * NB: this is NOT standard CRC32C.  The original byte-at-a-time table was
 * built with the non-reflected Castagnoli constant 0x1EDC6F41 in a
 * right-shift (reflected) table builder -- standard CRC32C uses the
 * bit-reversed constant 0x82F63B78 there, and the two disagree
 * ("123456789" -> 0xf28417be vs the standard 0xe3069283).  The values this
 * function produces are what every existing on-disk checksum holds, so the
 * constant is frozen: on-disk compatibility outranks mathematical purity.
 * In particular the kernel's __crc32c_le / libcrc32c cannot be substituted
 * (verified the hard way: a __crc32c_le build -EIOs on every old image).
 *
 * The recurrence itself is the ordinary reflected byte-at-a-time CRC
 * (register >> 8 ^ table[(register ^ byte) & 0xff]), so it is accelerated
 * with the standard slicing-by-8 technique -- exactly equivalent by
 * construction, including multi-part chaining, and ~4-8x faster than the
 * byte loop it replaced.  briefs_crc32c(crc, data, len) chains externally:
 * briefs_crc32c(briefs_crc32c(a, d1), d2) == briefs_crc32c(a, d1||d2).
 *
 * The 8 slice tables are built once at module init (no lazy-init race).
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/unaligned.h>
#include "briefs.h"

static u32 crc32c_slice[8][256];

/*
 * Build the slicing-by-8 tables from the frozen polynomial.
 * slice[0] is the classic byte-at-a-time table; slice[n][i] advances
 * one byte further: slice[n][i] = (slice[n-1][i] >> 8) ^ slice[0][slice[n-1][i] & 0xff].
 */
void briefs_crc32c_init(void)
{
	const u32 poly = 0x1EDC6F41;  /* frozen -- see file comment */
	int i, j, n;

	for (i = 0; i < 256; i++) {
		u32 crc = i;

		for (j = 0; j < 8; j++)
			crc = (crc & 1) ? (crc >> 1) ^ poly : crc >> 1;
		crc32c_slice[0][i] = crc;
	}
	for (n = 1; n < 8; n++)
		for (i = 0; i < 256; i++) {
			u32 prev = crc32c_slice[n - 1][i];

			crc32c_slice[n][i] = (prev >> 8) ^
				crc32c_slice[0][prev & 0xFF];
		}
}

/*
 * Compute the checksum.  Equivalent to the original byte loop:
 *   r = ~crc;  while (len--) r = (r >> 8) ^ table[(r ^ *buf++) & 0xFF];  ~r
 */
u32 briefs_crc32c(u32 crc, const void *data, size_t len) {
	const u8 *p = data;
	const u32 (*const s)[256] = crc32c_slice;
	u32 r;

	if (!data || len == 0)
		return crc;

	r = ~crc;
	while (len >= 8) {
		u32 one = get_unaligned_le32(p) ^ r;
		u32 two = get_unaligned_le32(p + 4);

		r = s[7][ one        & 0xFF] ^ s[6][(one >>  8) & 0xFF] ^
		    s[5][(one >> 16) & 0xFF] ^ s[4][(one >> 24) & 0xFF] ^
		    s[3][ two        & 0xFF] ^ s[2][(two >>  8) & 0xFF] ^
		    s[1][(two >> 16) & 0xFF] ^ s[0][(two >> 24) & 0xFF];
		p += 8;
		len -= 8;
	}
	while (len--)
		r = (r >> 8) ^ s[0][(r ^ *p++) & 0xFF];

	return ~r;
}