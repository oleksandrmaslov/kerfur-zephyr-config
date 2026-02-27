#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app/app.h"
#include "behavior/behavior_engine.h"
#include "ble/ble_manager.h"
#include "core/event_bus.h"
#include "drivers/mock_inputs.h"
#include "power/power_manager.h"
#include "ui/ui_renderer.h"

LOG_MODULE_REGISTER(kerfur_app, CONFIG_LOG_DEFAULT_LEVEL);

int app_run(void)
{
	struct pet_state pet;
	int64_t now_ms;
	int64_t last_frame_ms;
	int err;

	err = app_event_bus_init();
	if (err) {
		LOG_ERR("Event bus init failed (%d)", err);
		return err;
	}

	now_ms = k_uptime_get();
	behavior_engine_init(&pet, now_ms);
	power_manager_init(now_ms);

	err = ui_renderer_init();
	if (err) {
		LOG_ERR("UI init failed (%d)", err);
		return err;
	}

	err = mock_inputs_init();
	if (err) {
		LOG_WRN("Mock inputs init issue (%d), continuing", err);
	}

	err = ble_manager_init();
	if (err) {
		LOG_WRN("BLE scaffold not running (%d), continuing", err);
	}

	(void)app_event_publish(APP_EVENT_WAKE, 0);
	last_frame_ms = now_ms;

	while (1) {
		struct app_event event;
		struct app_event synthetic_event;
		bool got_event;
		bool display_blanked;

		got_event = app_event_wait(&event, K_MSEC(20));
		if (got_event) {
			behavior_engine_handle_event(&pet, &event);
			power_manager_on_event(&event, &pet);
		}

		now_ms = k_uptime_get();
		while (power_manager_poll(now_ms, &synthetic_event)) {
			(void)app_event_publish_with_timestamp(synthetic_event.type, synthetic_event.param,
							       synthetic_event.timestamp_ms);
		}

		if ((now_ms - last_frame_ms) < CONFIG_KERFUR_UI_FRAME_MS) {
			continue;
		}

		display_blanked = power_manager_is_display_blanked();
		ui_renderer_set_blanked(display_blanked);
		if (!display_blanked) {
			ui_renderer_render(&pet, now_ms);
		}

		last_frame_ms = now_ms;
	}
}
