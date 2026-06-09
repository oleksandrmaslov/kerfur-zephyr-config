/*
 * Kerfur IQS7222A configuration blob — PLACEHOLDER.
 *
 * TODO(hw): replace this file with the export from the Azoteq IQS7222A
 * Configuration Tool, tuned for the Kerfur petting-flex electrodes. Format the
 * export as iqs7222a_config_block entries (16-bit address + bytes). Until then
 * the array is empty and the driver runs the chip on its power-up defaults.
 */

#include <zephyr/sys/util.h>

#include "drivers/kerfur_iqs7222a_config.h"

const struct iqs7222a_config_block kerfur_iqs7222a_config[] = {
	/* { 0x8000, cycle_setup_0_bytes, sizeof(cycle_setup_0_bytes) }, ... */
};

const size_t kerfur_iqs7222a_config_count = ARRAY_SIZE(kerfur_iqs7222a_config);
