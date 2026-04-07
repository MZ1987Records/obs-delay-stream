#pragma once

#include <obs-module.h>

namespace ods::plugin {
	struct DelayStreamData;
}

namespace ods::ui::delay {

	/// サブチャンネル共通オフセット UI グループを追加する
	void add_master_offset_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

	/// 遅延サマリ UI グループを追加する
	void add_delay_summary_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

} // namespace ods::ui::delay
