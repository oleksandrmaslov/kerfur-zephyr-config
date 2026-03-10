#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app/app.h"
#include "behavior/behavior_engine.h"
#include "ble/ble_manager.h"
#include "core/event_bus.h"
#include "display/display_policy.h"
#include "drivers/mock_inputs.h"
#include "drivers/touch_input.h"
#include "power/power_manager.h"
#include "ui/ui_renderer.h"

LOG_MODULE_REGISTER(kerfur_app, CONFIG_LOG_DEFAULT_LEVEL);

#define APP_EVENT_DRAIN_BUDGET 32

static void log_pet_snapshot(const struct pet_state *pet, const char *tag)
{
	char status[256];

	behavior_engine_status_dump(pet, status, sizeof(status));
	LOG_INF("%s %s", tag, status);
}

static void app_handle_event(struct pet_state *pet, const struct app_event *event)
{
	if (IS_ENABLED(CONFIG_KERFUR_TRACE_EVENTS) &&
	    (event->type != APP_EVENT_TICK_100MS) &&
	    (event->type != APP_EVENT_TICK_1S)) {
		LOG_INF("Event rx: %s param=%d", app_event_type_str(event->type), event->param);
	}

	behavior_engine_handle_event(pet, event);
	power_manager_on_event(event, pet);
	display_policy_on_event(pet, event);
}

int app_run(void)
{
	struct pet_state pet;
	int64_t now_ms;
	int64_t last_frame_ms;
	int64_t last_state_log_ms;
	bool last_display_blanked;
	int err;

	err = app_event_bus_init();
	if (err) {
		LOG_ERR("Event bus init failed (%d)", err);
		return err;
	}

	now_ms = k_uptime_get();
	behavior_engine_init(&pet, now_ms);
	power_manager_init(now_ms);
	display_policy_init(&pet, now_ms);
	log_pet_snapshot(&pet, "Boot");

	err = ui_renderer_init();
	if (err) {
		LOG_ERR("UI init failed (%d)", err);
		return err;
	}

	err = mock_inputs_init();
	if (err) {
		LOG_WRN("Mock inputs init issue (%d), continuing", err);
	}

	err = touch_input_init();
	if (err) {
		LOG_WRN("Touch input init issue (%d), continuing", err);
	}

	err = ble_manager_init();
	if (err) {
		LOG_WRN("BLE scaffold not running (%d), continuing", err);
	}

	(void)app_event_publish(APP_EVENT_WAKE, 0);
	last_frame_ms = now_ms;
	last_state_log_ms = now_ms;
	last_display_blanked = false;
	LOG_INF("App main loop started");

	while (1) {
		struct app_event event;
		struct app_event synthetic_event;
		bool got_event;
		bool display_blanked = false;
		uint8_t drained = 0U;

		got_event = app_event_wait(&event, K_MSEC(20));
		if (got_event) {
			app_handle_event(&pet, &event);
			drained++;
		}

		while ((drained < APP_EVENT_DRAIN_BUDGET) &&
		       app_event_wait(&event, K_NO_WAIT)) {
			app_handle_event(&pet, &event);
			drained++;
		}

		now_ms = k_uptime_get();
		while (power_manager_poll(now_ms, &synthetic_event)) {
			(void)app_event_publish_with_timestamp(synthetic_event.type, synthetic_event.param,
							       synthetic_event.timestamp_ms);
		}

		if ((now_ms - last_frame_ms) < CONFIG_KERFUR_UI_FRAME_MS) {
			continue;
		}

		display_blanked = (display_policy_get_state(&pet) == DISPLAY_OFF);
		if (display_blanked != last_display_blanked) {
			LOG_INF("Display blank state -> %s", display_blanked ? "BLANKED" : "ACTIVE");
			last_display_blanked = display_blanked;
		}
		ui_renderer_set_blanked(display_blanked);
		if (!display_blanked) {
			ui_renderer_render(&pet, now_ms);
		}

		if (IS_ENABLED(CONFIG_KERFUR_TRACE_EVENTS) &&
		    ((now_ms - last_state_log_ms) >= (5 * MSEC_PER_SEC))) {
			log_pet_snapshot(&pet, "Heartbeat");
			last_state_log_ms = now_ms;
		}

		last_frame_ms = now_ms;
	}
}
