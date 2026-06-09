/*
 * Battery / charger monitor.
 *
 * Owns the active battery_backend, polls it on a slow cadence, and publishes
 * the BATTERY_* / CHARGER_* app events that the behavior, face and nearby
 * layers already consume. Battery percent/voltage come from the fuel gauge;
 * charger-present and charge-state come from the charger (see battery.h).
 *
 * The default "stub" backend reports everything as unknown, so until real
 * hardware + devicetree wiring exist the firmware behaves exactly as before.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "core/event_bus.h"
#include "power/battery.h"

LOG_MODULE_REGISTER(battery_monitor, CONFIG_LOG_DEFAULT_LEVEL);

#define BATTERY_POLL_MS CONFIG_KERFUR_BATTERY_POLL_MS

struct battery_monitor_state {
	const struct battery_backend *backend;
	struct battery_reading last;
	int64_t last_poll_ms;
	bool have_reading;
	bool charger_present_known;
	bool otg_enabled;
};

static struct battery_monitor_state g_bat;

/* ── Stub backend: no hardware, everything unknown ─────────────────── */

static int stub_init(void)
{
	return 0;
}

static int stub_read(struct battery_reading *out)
{
	memset(out, 0, sizeof(*out));
	out->percent_known = false;
	out->charger_present = false;
	out->charge_state = BATTERY_CHARGE_UNKNOWN;
	return 0;
}

static bool stub_supports_otg(void)
{
	return false;
}

static const struct battery_backend stub_backend = {
	.name = "stub",
	.init = stub_init,
	.read = stub_read,
	.set_otg = NULL,
	.supports_otg = stub_supports_otg,
};

const struct battery_backend *battery_backend_get(void)
{
#if defined(CONFIG_KERFUR_CHARGER_BQ2563X)
	return bq25630_backend_get();
#else
	return &stub_backend;
#endif
}

/* ── Monitor ───────────────────────────────────────────────────────── */

int battery_monitor_init(int64_t now_ms)
{
	int err;

	memset(&g_bat, 0, sizeof(g_bat));
	g_bat.backend = battery_backend_get();
	g_bat.last_poll_ms = now_ms - BATTERY_POLL_MS; /* poll on first tick */

	if (g_bat.backend == NULL || g_bat.backend->init == NULL) {
		LOG_WRN("Battery monitor: no backend");
		return -ENODEV;
	}

	err = g_bat.backend->init();
	if (err != 0) {
		LOG_WRN("Battery backend '%s' init failed (%d); running blind",
			g_bat.backend->name, err);
		/* Keep the stub-like behavior: poll will just read unknowns. */
	} else {
		LOG_INF("Battery monitor backend '%s' ready (otg=%d)",
			g_bat.backend->name,
			g_bat.backend->supports_otg &&
				g_bat.backend->supports_otg() ? 1 : 0);
	}

	return 0;
}

void battery_monitor_poll(int64_t now_ms)
{
	struct battery_reading r;
	int err;

	if (g_bat.backend == NULL || g_bat.backend->read == NULL) {
		return;
	}
	if ((now_ms - g_bat.last_poll_ms) < BATTERY_POLL_MS) {
		return;
	}
	g_bat.last_poll_ms = now_ms;

	err = g_bat.backend->read(&r);
	if (err != 0) {
		return;
	}

	/* Charger presence edges -> CHARGER_CONNECTED / DISCONNECTED. */
	if (!g_bat.charger_present_known) {
		/* First reading just establishes the baseline. Announce a
		 * charger that is genuinely present, but do NOT emit a spurious
		 * DISCONNECTED for the common "nothing plugged in at boot" case
		 * (which would also fire with the stub backend and perturb the
		 * behavior engine). */
		g_bat.charger_present_known = true;
		if (r.charger_present) {
			(void)app_event_publish_with_timestamp(
				APP_EVENT_CHARGER_CONNECTED, 0, now_ms);
		}
	} else if (r.charger_present != g_bat.last.charger_present) {
		(void)app_event_publish_with_timestamp(
			r.charger_present ? APP_EVENT_CHARGER_CONNECTED
					  : APP_EVENT_CHARGER_DISCONNECTED,
			0, now_ms);
	}

	/* State-of-charge -> BATTERY_PERCENT_UPDATE on change. The behavior
	 * engine derives low/critical from the percent payload, so we do not
	 * separately emit BATTERY_LOW/CRITICAL here. */
	if (r.percent_known &&
	    (!g_bat.have_reading || !g_bat.last.percent_known ||
	     (r.percent != g_bat.last.percent))) {
		(void)app_event_publish_battery_percent_with_timestamp(
			r.percent, true, now_ms);
	} else if (!r.percent_known && g_bat.have_reading &&
		   g_bat.last.percent_known) {
		(void)app_event_publish_battery_percent_with_timestamp(
			-1, false, now_ms);
	}

	r.otg_enabled = g_bat.otg_enabled;
	g_bat.last = r;
	g_bat.have_reading = true;
}

bool battery_monitor_get(struct battery_reading *out)
{
	if (out == NULL || !g_bat.have_reading) {
		return false;
	}
	*out = g_bat.last;
	return true;
}

int battery_monitor_set_otg(bool enable)
{
	int err;

	if (g_bat.backend == NULL || g_bat.backend->set_otg == NULL ||
	    g_bat.backend->supports_otg == NULL ||
	    !g_bat.backend->supports_otg()) {
		return -ENOTSUP;
	}

	err = g_bat.backend->set_otg(enable);
	if (err == 0) {
		g_bat.otg_enabled = enable;
		LOG_INF("OTG / powerbank %s", enable ? "ENABLED" : "disabled");
	} else {
		LOG_WRN("OTG set(%d) failed (%d)", enable ? 1 : 0, err);
	}
	return err;
}

bool battery_monitor_otg_enabled(void)
{
	return g_bat.otg_enabled;
}
