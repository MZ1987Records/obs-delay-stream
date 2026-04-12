#pragma once

#include <obs-module.h>

namespace ods::plugin {
	struct DelayStreamData;
}

namespace ods::ui {

	/// プラグイン情報 UI グループを追加する
	void add_plugin_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

	/// タブ選択ボタン行を追加する
	void add_tab_selector_row(obs_properties_t *props, ods::plugin::DelayStreamData *d, int active_tab);

	/// 配信情報 UI グループを追加する
	void add_stream_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

	/// WebSocket 設定 UI グループを追加する
	void add_ws_group(obs_properties_t *props, ods::plugin::DelayStreamData *d, bool has_sid);

	/// cloudflared トンネル UI グループを追加する
	void add_tunnel_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

	/// 同期フロー UI グループを追加する
	void add_flow_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

	/// マスターディレイ UI グループを追加する
	void add_master_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

} // namespace ods::ui
