#ifndef KERFUR_IQS7222A_CONFIG_H_
#define KERFUR_IQS7222A_CONFIG_H_

#include <stddef.h>
#include <stdint.h>

/*
 * IQS7222A register configuration blocks pushed at init.
 *
 * Each block writes `len` bytes starting at a 16-bit memory-map address (the
 * config registers live at 0x8000+, see IQS7222A_addresses.h). The contents
 * are specific to the electrode geometry of the Kerfur petting flex and must
 * come from the Azoteq IQS7222A Configuration Tool export for that board.
 *
 * Until that export exists, the array is EMPTY: the driver then leaves the chip
 * on its power-up defaults. Touch reads still work structurally, but channel
 * mapping and sensitivity are NOT tuned for the flex. Replace
 * kerfur_iqs7222a_config.c with the generated config to light it up properly.
 */
struct iqs7222a_config_block {
	uint16_t addr;          /* 16-bit memory-map address, e.g. 0x8000 */
	const uint8_t *data;
	uint8_t len;
};

extern const struct iqs7222a_config_block kerfur_iqs7222a_config[];
extern const size_t kerfur_iqs7222a_config_count;

#endif /* KERFUR_IQS7222A_CONFIG_H_ */
