/* Host-test shim for <zephyr/settings/settings.h>. The nearby tests run with
 * no persisted state: load is a no-op (so no friends / fresh secret), saves are
 * dropped. Just enough surface for src/nearby/kerfur_nearby.c to compile and
 * link, with the static settings handler kept "referenced" to avoid a warning. */
#ifndef KERFUR_NEARBY_HOST_SHIM_SETTINGS_H_
#define KERFUR_NEARBY_HOST_SHIM_SETTINGS_H_

#include <stddef.h>
#include <string.h>
#include <sys/types.h>   /* ssize_t */

typedef ssize_t (*settings_read_cb)(void *cb_arg, void *data, size_t len);

static inline int settings_subsys_init(void) { return 0; }
static inline int settings_load_subtree(const char *subtree)
{
	(void)subtree;
	return 0;
}
static inline int settings_save_one(const char *name, const void *value, size_t val_len)
{
	(void)name; (void)value; (void)val_len;
	return 0;
}

/* Minimal name matcher: true when `name` equals `key` or is `key/<rest>`,
 * setting *next to <rest> in the latter case (mirrors Zephyr semantics). */
static inline int settings_name_steq(const char *name, const char *key, const char **next)
{
	size_t kl = strlen(key);

	if (next != NULL) {
		*next = NULL;
	}
	if (strncmp(name, key, kl) != 0) {
		return 0;
	}
	if (name[kl] == '\0') {
		return 1;
	}
	if (name[kl] == '/') {
		if (next != NULL) {
			*next = &name[kl + 1];
		}
		return 1;
	}
	return 0;
}

/* Reference the set-handler so the real (static) handler isn't flagged unused;
 * the prototype matches Zephyr's settings_set callback exactly. */
#define SETTINGS_STATIC_HANDLER_DEFINE(_name, _tree, _get, _set, _commit, _export) \
	static int (*const _name##_shim_set_ref)(const char *, size_t,          \
						 settings_read_cb, void *)     \
		__attribute__((unused)) = (_set)

#endif /* KERFUR_NEARBY_HOST_SHIM_SETTINGS_H_ */
