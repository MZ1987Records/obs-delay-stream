#pragma once

#include <obs-module.h>

namespace ods::plugin {
	struct DelayStreamData;
}
namespace ods::viewmodel {
	struct DelayViewModel;
}

namespace ods::ui::delay {

	/// ローカル生演奏 UI グループを追加する。
	void add_live_perf_group(obs_properties_t *props, ods::plugin::DelayStreamData *d, const ods::viewmodel::DelayViewModel &vm);

	/// 全体調整 UI グループを追加する（アバター遅延ステッパー＋タイミング図）
	void add_delay_diagram_group(obs_properties_t *props, ods::plugin::DelayStreamData *d, const ods::viewmodel::DelayViewModel &vm);

	/// 微調整 UI グループを追加する（アバターレイテンシ＋環境レイテンシ）
	void add_fine_tune_group(obs_properties_t *props, ods::plugin::DelayStreamData *d, const ods::viewmodel::DelayViewModel &vm);

} // namespace ods::ui::delay
