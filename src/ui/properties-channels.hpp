#pragma once

#include <obs-module.h>

namespace ods::plugin {
	struct DelayStreamData;
}

namespace ods::ui::channels {

	/// サブチャンネル設定 UI グループを追加する
	void add_sub_channels_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

} // namespace ods::ui::channels
