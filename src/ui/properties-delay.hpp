#pragma once

#include <obs-module.h>

namespace ods::plugin {
	struct DelayStreamData;
}
namespace ods::viewmodel {
	struct DelayViewModel;
}

namespace ods::ui::delay {

	/// 遅延タイミング図 UI グループを追加する（ViewModel 経由）
	void add_delay_diagram_group(obs_properties_t *props, const ods::viewmodel::DelayViewModel &vm);

	/// 微調整 UI グループを追加する（アバターレイテンシ＋環境レイテンシ）
	void add_fine_tune_group(obs_properties_t *props, ods::plugin::DelayStreamData *d, const ods::viewmodel::DelayViewModel &vm);

} // namespace ods::ui::delay
