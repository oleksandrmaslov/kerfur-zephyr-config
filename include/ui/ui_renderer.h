#ifndef KERFUR_UI_RENDERER_H_
#define KERFUR_UI_RENDERER_H_

#include <stdbool.h>
#include <stdint.h>

#include "behavior/pet_state.h"

int ui_renderer_init(void);
void ui_renderer_set_blanked(bool blanked);
void ui_renderer_render(const struct pet_state *state, int64_t now_ms);

#endif /* KERFUR_UI_RENDERER_H_ */
