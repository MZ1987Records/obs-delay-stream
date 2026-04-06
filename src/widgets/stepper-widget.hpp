#pragma once

#include <obs-module.h>

namespace ods::widgets {

obs_property_t *obs_properties_add_stepper(
	obs_properties_t *props,
	const char       *prop_name,
	const char       *label,
	const char       *setting_key,
	double            min_val,
	double            max_val,
	double            def_val,
	int               decimals,
	const char       *suffix,
	bool              store_as_int    = false,
	int               max_input_chars = 7);

void schedule_stepper_inject(obs_source_t *source);

} // namespace ods::widgets
