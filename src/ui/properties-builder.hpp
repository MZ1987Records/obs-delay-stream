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

	/// タブ選択コールバック（TabCtx::tab に移動してプロパティを再描画する）
	bool cb_select_tab(obs_properties_t *, obs_property_t *, void *priv);

	/// 指定タブへ移動する「次へ」ボタンバーを追加する
	void add_next_tab_button_bar(obs_properties_t *props, ods::plugin::DelayStreamData *d, int next_tab, bool enabled = true);

	/// WS配信タブ用: 未起動なら「起動して次へ」、起動済みなら「次へ」を表示する
	void add_ws_next_button_bar(obs_properties_t *props, ods::plugin::DelayStreamData *d, bool has_sid);

	/// トンネルタブ用: 停止中なら「起動して次へ」+「スキップ」、起動済みなら「次へ」を表示する
	void add_tunnel_next_button_bar(obs_properties_t *props, ods::plugin::DelayStreamData *d);

} // namespace ods::ui
