#pragma once

#include <string>

namespace ods::plugin {

	/**
	 * 最新リリース取得結果。
	 */
	struct LatestReleaseInfo {
		std::string latest_version; ///< 最新バージョン文字列
		std::string release_url;    ///< リリース URL
		std::string error;          ///< 失敗時のエラー内容
	};

	/// GitHub から最新リリース情報を取得する
	bool fetch_latest_release_info(LatestReleaseInfo &out);

	/// `latest` が `current` より新しいか判定する
	bool is_newer_version(const std::string &latest, const std::string &current);

} // namespace ods::plugin
