#ifndef KERFUR_BATTERY_H_
#define KERFUR_BATTERY_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Battery / charger monitor.
 *
 * Kerfur's power front-end is two I2C devices (see "case and pcb" schematic):
 *   - MAX17048 fuel gauge  -> state-of-charge percent + cell voltage + ALRT
 *   - BQ25630 charger      -> charger-present (PG), charge state (/INT/STAT),
 *                             and OTG / powerbank boost control
 *
 * This module hides that behind a single backend interface and owns the
 * publishing of the BATTERY_* / CHARGER_* app events the behavior, face and
 * nearby layers already consume. A "stub" backend reports everything as
 * unknown so the firmware behaves exactly as before until real hardware and
 * pin/address wiring are finalized.
 */

enum battery_charge_state {
	BATTERY_CHARGE_UNKNOWN = 0,
	BATTERY_CHARGE_DISCHARGING,
	BATTERY_CHARGE_CHARGING,
	BATTERY_CHARGE_FULL,
	BATTERY_CHARGE_FAULT,
};

struct battery_reading {
	bool percent_known;             /* true if `percent` is valid */
	int8_t percent;                 /* 0..100 state of charge (fuel gauge) */
	uint16_t millivolts;            /* cell voltage, 0 if unknown */
	bool charger_present;           /* VBUS / PG asserted on the charger */
	enum battery_charge_state charge_state;
	bool otg_enabled;               /* OTG / powerbank boost output active */
};

/*
 * Backend abstraction. A backend may source any subset of the fields; whatever
 * it cannot provide it must leave as "unknown" (percent_known=false, etc.).
 * Real backend = MAX17048 gauge + BQ25630 charger sharing one I2C bus.
 */
struct battery_backend {
	const char *name;
	int  (*init)(void);
	int  (*read)(struct battery_reading *out);
	int  (*set_otg)(bool enable);   /* enable/disable powerbank boost */
	bool (*supports_otg)(void);
};

/* Returns the active backend for this build (selected via Kconfig). */
const struct battery_backend *battery_backend_get(void);

/* Lifecycle / runtime (event publishing is owned here). */
int  battery_monitor_init(int64_t now_ms);
void battery_monitor_poll(int64_t now_ms);

/* Last cached reading. Returns false if the monitor has no valid reading. */
bool battery_monitor_get(struct battery_reading *out);

/* Powerbank / OTG control (intended to be driven by a button press, not by
 * continuous USB polling, to keep idle current low). Returns 0 on success,
 * -ENOTSUP if the backend has no OTG path. */
int  battery_monitor_set_otg(bool enable);
bool battery_monitor_otg_enabled(void);

#endif /* KERFUR_BATTERY_H_ */
