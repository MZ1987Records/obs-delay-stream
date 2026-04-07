#pragma once

#include <obs-module.h>

namespace ods::plugin {
	struct DelayStreamData;
}

namespace ods::ui {

	/// プラグイン情報 UI グループを追加する
	void add_plugin_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

	/// 配信情報 UI グループを追加する
	void add_stream_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

	/// WebSocket 設定 UI グループを追加する
	void add_ws_group(obs_properties_t *props, ods::plugin::DelayStreamData *d, bool has_sid);

	/// cloudflared トンネル UI グループを追加する
	void add_tunnel_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

	/// 同期フロー UI グループを追加する
	void add_flow_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

	/// マスター遅延 UI グループを追加する
	void add_master_group(obs_properties_t *props, ods::plugin::DelayStreamData *d);

} // namespace ods::ui
