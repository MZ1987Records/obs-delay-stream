#pragma once

#include <obs-module.h>

namespace ods::widgets {

	obs_property_t *obs_properties_add_text_button(
		obs_properties_t      *props,
		const char            *prop_name,
		const char            *label,
		const char            *setting_key,
		const char            *button_label,
		obs_property_clicked_t clicked,
		void                  *clicked_priv    = nullptr,
		bool                   input_enabled   = true,
		bool                   button_enabled  = true,
		int                    max_input_chars = 0);

	void schedule_text_button_inject(obs_source_t *source);

} // namespace ods::widgets
