#pragma once

#include <obs-module.h>
#include <string>

namespace ods::plugin {

	/// OBS ソースが削除済みか判定する
	bool is_obs_source_removed(obs_source_t *source);

	/// OBS の配信出力が現在アクティブか判定する
	bool is_obs_streaming_active();

	/// OBS の出力設定から RTMP URL を取得する
	std::string get_obs_stream_url();

	/// 設定に RTMP URL を自動補完する
	void maybe_autofill_rtmp_url(obs_data_t *settings, bool force_refresh);

	/// ソース設定に RTMP URL を自動補完する
	void maybe_autofill_rtmp_url_from_source(obs_source_t *source, bool force_refresh);

} // namespace ods::plugin
