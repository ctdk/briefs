/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/*
 * CRC32C implementation (Castagnoli polynomial)
 * Used for journal record checksums
 */

#include <stdint.h>
#include <stddef.h>

static uint32_t crc32c_table[256];
static bool crc32c_table_initialized = false;

/*
 * Initialize CRC32C lookup table
 */
static void crc32c_init(void) {
	if (crc32c_table_initialized) return;

	const uint32_t poly = 0x1EDC6F41;  /* Castagnoli polynomial */

	for (uint32_t i = 0; i < 256; i++) {
		uint32_t crc = i;
		for (int j = 0; j < 8; j++) {
			if (crc & 1) {
				crc = (crc >> 1) ^ poly;
			} else {
				crc >>= 1;
			}
		}
		crc32c_table[i] = crc;
	}

	crc32c_table_initialized = true;
}

/*
 * Compute CRC32C checksum
 */
uint32_t briefs_crc32c(uint32_t crc, const void *data, size_t len) {
	if (!crc32c_table_initialized) {
		crc32c_init();
	}

	if (!data || len == 0) return crc;

	const uint8_t *buf = (const uint8_t *)data;

	/* CRC is computed LSB first, invert at end */
	crc = ~crc;

	while (len--) {
		uint8_t index = (crc ^ *buf++) & 0xFF;
		crc = (crc >> 8) ^ crc32c_table[index];
	}

	return ~crc;
}
