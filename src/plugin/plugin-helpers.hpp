#pragma once

#include <obs-module.h>
#include <string>

namespace ods::plugin {

	/// ソースの設定から RTMP URL を解決する
	std::string resolve_rtmp_url_from_source(obs_source_t *source);

	/// cloudflared パスが未設定なら自動検索結果を補完する
	void maybe_fill_cloudflared_path_from_auto(obs_source_t *source);

	/// 自動検索完了後に cloudflared パスを設定に保存する
	void maybe_persist_cloudflared_path_after_auto_ready(obs_source_t *source);

} // namespace ods::plugin
