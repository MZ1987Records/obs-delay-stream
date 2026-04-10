#pragma once

#include <obs-module.h>

namespace ods::plugin {
	struct DelayStreamData;
}
namespace ods::viewmodel {
	struct DelayViewModel;
}

namespace ods::ui::delay {

	/// アバターレイテンシ設定 UI グループを追加する
	void add_avatar_latency_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

	/// 遅延サマリ UI グループを追加する（ViewModel 経由）
	void add_delay_summary_group(obs_properties_t *props, const ods::viewmodel::DelayViewModel &vm);

} // namespace ods::ui::delay
