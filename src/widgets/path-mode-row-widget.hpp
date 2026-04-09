#pragma once

#include <obs-module.h>

namespace ods::widgets {

	struct ObsPathModeRowSpec {
		const char            *mode_setting_key      = nullptr;
		const char            *path_setting_key      = nullptr;
		const char            *auto_mode_label       = nullptr;
		const char            *path_mode_label       = nullptr;
		const char            *absolute_mode_label   = nullptr;
		const char            *download_button_label = nullptr;
		obs_property_clicked_t download_clicked      = nullptr;
		void                  *download_clicked_priv = nullptr;
		bool                   download_enabled      = true;
		bool                   manual_enabled        = true;
		int                    max_input_chars       = 0;
		bool                   auto_path_exists      = false;
		const char            *auto_path_display     = nullptr;
	};

	obs_property_t *obs_properties_add_path_mode_row(
		obs_properties_t         *props,
		const char               *prop_name,
		const char               *label,
		const ObsPathModeRowSpec &spec);

	void schedule_path_mode_row_inject(obs_source_t *source);

} // namespace ods::widgets
