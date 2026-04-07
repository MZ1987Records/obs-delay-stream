#pragma once

#include <string>
#include <vector>

namespace ods::ui {

	/**
	 * URL 共有表示の 1 行分データ。
	 */
	struct UrlShareRow {
		int         ch_1indexed = 0; ///< 1-indexed チャンネル番号
		std::string name;            ///< 表示名
		std::string url;             ///< 共有 URL
	};

	/**
	 * URL 確認 HTML のテーマ色定義。
	 */
	struct UrlConfirmThemeColors {
		std::string table_bg;    ///< テーブル背景色
		std::string header_bg;   ///< ヘッダー背景色
		std::string header_text; ///< ヘッダーテキスト色
		std::string row_bg;      ///< 行背景色
		std::string alt_row_bg;  ///< 交互行背景色
		std::string text;        ///< 通常テキスト色
		std::string border;      ///< 罫線色
		std::string link;        ///< リンク色
	};

	/// 共有用のプレーンテキストを生成する
	std::string build_url_share_copy_text(const std::vector<UrlShareRow> &rows,
										  const char                     *not_configured_text);

	/// URL 確認ダイアログ用の HTML 文字列を生成する
	std::string build_url_confirm_html_text(const std::vector<UrlShareRow> &rows,
											const char                     *not_configured_text,
											const UrlConfirmThemeColors    *theme_colors = nullptr,
											bool                            linkify_urls = true);

} // namespace ods::ui
