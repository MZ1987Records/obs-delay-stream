#pragma once

#include <obs-module.h>

namespace ods::widgets {

	/// プレースホルダーとして OBS_TEXT_INFO プロパティを追加する。inject 後にテーマ追従するコールアウトに置換される。
	obs_property_t *obs_properties_add_help_callout(
		obs_properties_t *props,
		const char       *prop_name,
		const char       *text);

	void schedule_help_callout_inject(obs_source_t *source); ///< プレースホルダーをコールアウトウィジェットへ差し替える

} // namespace ods::widgets
