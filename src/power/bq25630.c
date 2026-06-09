/*
 * Real battery backend: TI BQ25630 charger + Maxim MAX17048 fuel gauge.
 *
 * Register facts below are taken directly from the BQ25630 datasheet
 * (assets/bq25630.pdf) and the MAX17048 datasheet:
 *
 *   BQ25630  I2C 7-bit address 0x6B
 *     REG0x16 Charger_Control_1  : EN_CHG (bit5), EN_HIZ (bit2)
 *     REG0x18 Charger_Control_3  : EN_OTG (bit6)  <- powerbank/OTG boost
 *     REG0x1F Charger_Status_0   : PG_STAT (bit7) <- charger present / power good
 *     REG0x20 Charger_Status_1   : CHG_STAT (bits 5:3)
 *                                    000 not charging, 001 trickle, 010 pre,
 *                                    011 fast, 1xx taper/top-off/done
 *     REG0x4D Part_Information    : used as a comms probe
 *
 *   MAX17048 I2C 7-bit address 0x36
 *     0x02 VCELL : cell voltage, 78.125 uV / LSB (big-endian)
 *     0x04 SOC   : state of charge, 1/256 %% / LSB (big-endian; high byte = %%)
 *
 * Both devices share the host I2C bus. Wire them in the board overlay as the
 * `charger0` and `fuel_gauge0` aliases; this file degrades to "unknown" when
 * those aliases are absent so it always compiles.
 *
 * TODO(hw): the schematic is currently unwired — the BQ25630 /INT and PG and
 * the MAX17048 ALRT GPIOs are not yet mapped to nRF52840 pins. When they are,
 * add interrupt-driven updates (and the OTG button) instead of pure polling.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "power/battery.h"

LOG_MODULE_REGISTER(bq25630, CONFIG_LOG_DEFAULT_LEVEL);

/* BQ25630 registers */
#define BQ_REG_CHARGER_CONTROL_1 0x16U
#define BQ_REG_CHARGER_CONTROL_3 0x18U
#define BQ_REG_CHARGER_STATUS_0  0x1FU
#define BQ_REG_CHARGER_STATUS_1  0x20U
#define BQ_REG_PART_INFORMATION  0x4DU

#define BQ_EN_OTG                BIT(6) /* Charger_Control_3 */
#define BQ_PG_STAT               BIT(7) /* Charger_Status_0  */
#define BQ_CHG_STAT_MASK         0x38U  /* Charger_Status_1, bits 5:3 */
#define BQ_CHG_STAT_SHIFT        3U

/* MAX17048 registers */
#define MAX17048_REG_VCELL       0x02U
#define MAX17048_REG_SOC         0x04U

#if DT_HAS_ALIAS(charger0)
static const struct i2c_dt_spec g_charger = I2C_DT_SPEC_GET(DT_ALIAS(charger0));
#define HAVE_CHARGER 1
#else
#define HAVE_CHARGER 0
#endif

#if DT_HAS_ALIAS(fuel_gauge0)
static const struct i2c_dt_spec g_gauge = I2C_DT_SPEC_GET(DT_ALIAS(fuel_gauge0));
#define HAVE_GAUGE 1
#else
#define HAVE_GAUGE 0
#endif

static bool g_charger_ready;
static bool g_gauge_ready;

#if HAVE_GAUGE
static int gauge_read_u16(uint8_t reg, uint16_t *out)
{
	uint8_t buf[2];
	int err = i2c_burst_read_dt(&g_gauge, reg, buf, sizeof(buf));

	if (err != 0) {
		return err;
	}
	*out = ((uint16_t)buf[0] << 8) | buf[1]; /* MAX17048 is big-endian */
	return 0;
}
#endif

static int bq_init(void)
{
	g_charger_ready = false;
	g_gauge_ready = false;

#if HAVE_CHARGER
	if (device_is_ready(g_charger.bus)) {
		uint8_t part;

		if (i2c_reg_read_byte_dt(&g_charger, BQ_REG_PART_INFORMATION,
					 &part) == 0) {
			g_charger_ready = true;
			LOG_INF("BQ25630 detected (part_info=0x%02x)", part);
		} else {
			LOG_WRN("BQ25630 not responding on I2C");
		}
	}
#endif
#if HAVE_GAUGE
	if (device_is_ready(g_gauge.bus)) {
		uint16_t soc;

		if (gauge_read_u16(MAX17048_REG_SOC, &soc) == 0) {
			g_gauge_ready = true;
			LOG_INF("MAX17048 detected (soc=%u%%)", soc >> 8);
		} else {
			LOG_WRN("MAX17048 not responding on I2C");
		}
	}
#endif

	if (!g_charger_ready && !g_gauge_ready) {
		return -ENODEV;
	}
	return 0;
}

static enum battery_charge_state map_chg_stat(uint8_t status1, bool present)
{
	uint8_t chg = (status1 & BQ_CHG_STAT_MASK) >> BQ_CHG_STAT_SHIFT;

	if (chg != 0U) {
		return BATTERY_CHARGE_CHARGING; /* trickle/pre/fast/taper */
	}
	/* Not actively charging: full if powered, otherwise running on battery. */
	return present ? BATTERY_CHARGE_FULL : BATTERY_CHARGE_DISCHARGING;
}

static int bq_read(struct battery_reading *out)
{
	memset(out, 0, sizeof(*out));
	out->charge_state = BATTERY_CHARGE_UNKNOWN;

#if HAVE_GAUGE
	if (g_gauge_ready) {
		uint16_t soc;
		uint16_t vcell;

		if (gauge_read_u16(MAX17048_REG_SOC, &soc) == 0) {
			out->percent = (int8_t)CLAMP(soc >> 8, 0, 100);
			out->percent_known = true;
		}
		if (gauge_read_u16(MAX17048_REG_VCELL, &vcell) == 0) {
			/* 78.125 uV/LSB == 5/64 mV/LSB. Use that exact ratio:
			 * the naive (vcell * 78125 / 1000000) overflows uint32
			 * for cell voltages above ~4.29 V. */
			out->millivolts = (uint16_t)(((uint32_t)vcell * 5U) / 64U);
		}
	}
#endif
#if HAVE_CHARGER
	if (g_charger_ready) {
		uint8_t st0;
		uint8_t st1 = 0U;

		if (i2c_reg_read_byte_dt(&g_charger, BQ_REG_CHARGER_STATUS_0,
					 &st0) == 0) {
			out->charger_present = (st0 & BQ_PG_STAT) != 0U;
			(void)i2c_reg_read_byte_dt(&g_charger,
						   BQ_REG_CHARGER_STATUS_1,
						   &st1);
			out->charge_state =
				map_chg_stat(st1, out->charger_present);
		}
	}
#endif
	return 0;
}

static int bq_set_otg(bool enable)
{
#if HAVE_CHARGER
	if (!g_charger_ready) {
		return -ENODEV;
	}
	/* Set/clear EN_OTG (REG0x18 bit6) to start/stop the powerbank boost. */
	return i2c_reg_update_byte_dt(&g_charger, BQ_REG_CHARGER_CONTROL_3,
				      BQ_EN_OTG, enable ? BQ_EN_OTG : 0U);
#else
	ARG_UNUSED(enable);
	return -ENOTSUP;
#endif
}

static bool bq_supports_otg(void)
{
	return HAVE_CHARGER && g_charger_ready;
}

static const struct battery_backend bq25630_backend = {
	.name = "bq25630+max17048",
	.init = bq_init,
	.read = bq_read,
	.set_otg = bq_set_otg,
	.supports_otg = bq_supports_otg,
};

const struct battery_backend *bq25630_backend_get(void)
{
	return &bq25630_backend;
}
