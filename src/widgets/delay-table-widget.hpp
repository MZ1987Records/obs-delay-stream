#pragma once

#include <obs-module.h>

namespace ods::widgets {

	/**
	 * ディレイテーブルウィジェットに渡す 1 チャンネル分の情報。
	 */
	struct DelayTableChannelInfo {
		const char *name;        ///< メモ名（空文字列可）
		float       measured_ms; ///< WS 配信遅延（片道 ms）。未計測は -1.0f
		int         offset_ms;   ///< 想定環境遅延（チャンネル別補正オフセット）
		int         total_ms;    ///< 自動調整ディレイ（バッファ適用値）
	};

	/**
	 * テーブル列ヘッダーとエディタラベルの文字列。
	 *
	 * plugin-main.cpp 側で `obs_module_text()` (`T_()`) を使って渡す。
	 */
	struct DelayTableLabels {
		const char *hdr_ch       = nullptr; ///< "Ch" 列ヘッダー
		const char *hdr_name     = nullptr; ///< "名前" 列ヘッダー
		const char *hdr_auto     = nullptr; ///< "自動調整" 列ヘッダー
		const char *hdr_ws       = nullptr; ///< "WS" 列ヘッダー
		const char *hdr_env      = nullptr; ///< "環境" 列ヘッダー
		const char *hdr_sum      = nullptr; ///< "合計" 列ヘッダー
		const char *lbl_editor   = nullptr; ///< エディタ行ラベル
		const char *editor_color = nullptr; ///< エディタラベル先頭の色付き四角（HEX、nullptr で非表示）
		const char *help_text    = nullptr; ///< ステッパー下部のヘルプテキスト
	};

	obs_property_t *obs_properties_add_delay_table(
		obs_properties_t            *props,
		const char                  *prop_name,
		int                          selected_ch,
		int                          ch_count,
		const DelayTableChannelInfo *channels,
		int                          sum_ms,
		const DelayTableLabels      &labels); ///< QFormLayout 用プレースホルダーとして OBS_TEXT_INFO プロパティを追加する

	void schedule_delay_table_inject(obs_source_t *source); ///< プレースホルダーを実ウィジェットへ差し替える inject を予約する

} // namespace ods::widgets
