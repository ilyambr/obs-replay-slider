#include "hotkey-lookup.hpp"

#include <util/dstr.h>

namespace {

struct FindHotkeyCtx {
	// obs_hotkey_register_output() stores the registerer as the output's weak
	// wrapper (obs_output_get_weak_output()), not the raw obs_output_t*, so
	// that's what has to be compared against here.
	obs_weak_output_t *targetWeak;
	obs_hotkey_id id;
};

bool FindHotkeyForOutput(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	auto *ctx = static_cast<FindHotkeyCtx *>(data);
	if (obs_hotkey_get_registerer_type(key) != OBS_HOTKEY_REGISTERER_OUTPUT)
		return true;
	if (obs_hotkey_get_registerer(key) != static_cast<void *>(ctx->targetWeak))
		return true;
	ctx->id = id;
	return false;
}

struct FindBindingCtx {
	obs_hotkey_id id;
	obs_key_combination_t combo;
	bool found;
};

bool FindBindingForHotkey(void *data, size_t idx, obs_hotkey_binding_t *binding)
{
	UNUSED_PARAMETER(idx);
	auto *ctx = static_cast<FindBindingCtx *>(data);
	if (obs_hotkey_binding_get_hotkey_id(binding) != ctx->id)
		return true;
	ctx->combo = obs_hotkey_binding_get_key_combination(binding);
	ctx->found = true;
	return false;
}

} // namespace

QString FindOutputHotkeyString(obs_output_t *output)
{
	if (!output)
		return {};

	obs_weak_output_t *weakOutput = obs_output_get_weak_output(output);
	FindHotkeyCtx findCtx{weakOutput, OBS_INVALID_HOTKEY_ID};
	obs_enum_hotkeys(FindHotkeyForOutput, &findCtx);
	if (weakOutput)
		obs_weak_output_release(weakOutput);
	if (findCtx.id == OBS_INVALID_HOTKEY_ID)
		return {};

	FindBindingCtx bindCtx{findCtx.id, {}, false};
	obs_enum_hotkey_bindings(FindBindingForHotkey, &bindCtx);
	if (!bindCtx.found)
		return {};

	struct dstr str = {0};
	obs_key_combination_to_str(bindCtx.combo, &str);
	QString result = str.array ? QString::fromUtf8(str.array) : QString();
	dstr_free(&str);
	return result;
}
