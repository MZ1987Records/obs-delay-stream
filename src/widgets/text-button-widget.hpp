#pragma once

#include <cstddef>
#include <obs-module.h>

namespace ods::widgets {

	struct ObsTextButtonActionSpec {
		const char            *button_label = nullptr;
		obs_property_clicked_t clicked      = nullptr;
		void                  *clicked_priv = nullptr;
		bool                   enabled      = true;
	};

	obs_property_t *obs_properties_add_text_buttons(
		obs_properties_t              *props,
		const char                    *prop_name,
		const char                    *label,
		const char                    *setting_key,
		const ObsTextButtonActionSpec *buttons,
		size_t                         button_count,
		bool                           input_enabled   = true,
		int                            max_input_chars = 0);

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

	obs_property_t *obs_properties_add_text_readonly(
		obs_properties_t *props,
		const char       *prop_name,
		const char       *label,
		const char       *setting_key);

	void schedule_text_button_inject(obs_source_t *source);

} // namespace ods::widgets
