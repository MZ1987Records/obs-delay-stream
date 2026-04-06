#pragma once

#include <cstddef>
#include <obs-module.h>

struct ObsColorButtonSpec {
	const char            *action_prop_name = nullptr;
	const char            *button_label     = nullptr;
	obs_property_clicked_t clicked          = nullptr;
	void                  *clicked_priv     = nullptr;
	bool                   enabled          = true;
	const char            *bg_color         = nullptr;
	const char            *text_color       = nullptr;
};

obs_property_t *obs_properties_add_color_button_row(
	obs_properties_t         *props,
	const char               *prop_name,
	const char               *label,
	const ObsColorButtonSpec *buttons,
	size_t                    button_count);

void schedule_color_button_row_inject(obs_source_t *source);
