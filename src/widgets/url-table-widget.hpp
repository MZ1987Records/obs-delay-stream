#pragma once

#include <obs-module.h>

namespace ods::widgets {

	/**
	 * URL 一覧テーブルウィジェットに渡す行データ。
	 *
	 * QPainter 描画で QFontMetrics::elidedText を使い、
	 * ウィジェット幅に応じて名前（右省略）・URL（左省略）を動的に切り詰める。
	 */
	struct UrlTableInfo {
		static constexpr int kMaxCh = 20;

		int  ch_count = 0;    ///< チャンネル数
		bool linkify  = true; ///< URL をクリッカブルにするか

		struct Row {
			int  ch_1indexed = 0; ///< 1-indexed チャンネル番号
			char name[128]{};     ///< 表示名（UTF-8）
			char url[512]{};      ///< 共有 URL（UTF-8）
		};
		Row rows[kMaxCh]{};

		char not_configured[64]{}; ///< 未設定時の表示テキスト
	};

	/// QFormLayout 用プレースホルダーとして OBS_TEXT_INFO プロパティを追加する
	obs_property_t *obs_properties_add_url_table(
		obs_properties_t   *props,
		const char         *prop_name,
		const UrlTableInfo &info);

	/// プレースホルダーを実ウィジェットへ差し替える inject を予約する
	void schedule_url_table_inject(obs_source_t *source);

} // namespace ods::widgets
