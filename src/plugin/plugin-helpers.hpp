#pragma once

#include <obs-module.h>
#include <cstdint>
#include <string>

namespace ods::plugin {

	/// フィルタソースの親ソースの音声同期オフセット(ns)を取得する
	bool try_get_parent_audio_sync_offset_ns(obs_source_t *filter_source, int64_t &out_offset_ns);

	/// ソースの設定から RTMP URL を解決する
	std::string resolve_rtmp_url_from_source(obs_source_t *source);

	/// cloudflared パスが未設定なら自動検索結果を補完する
	void maybe_fill_cloudflared_path_from_auto(obs_source_t *source);

	/// 自動検索完了後に cloudflared パスを設定に保存する
	void maybe_persist_cloudflared_path_after_auto_ready(obs_source_t *source);

} // namespace ods::plugin
