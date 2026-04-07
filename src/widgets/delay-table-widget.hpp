#pragma once

#include <obs-module.h>

namespace ods::widgets {

	/**
	 * 遅延テーブルウィジェットに渡す 1 チャンネル分の情報。
	 */
	struct DelayTableChannelInfo {
		const char *name;        ///< メモ名（空文字列可）
		float       measured_ms; ///< 計測値（片道）。未計測は -1.0f
		float       base_ms;     ///< ベース（proposed_delay）
		float       adjust_ms;   ///< アジャスト（adjust_ms）
		float       global_ms;   ///< 共通オフセット（sub_offset_ms）
		float       total_ms;    ///< 合計 = max(0, base + adjust + global)
		bool        warn;        ///< 生値 total が 0 未満なら true
	};

	/**
	 * テーブル列ヘッダーとエディタラベルの文字列。
	 *
	 * plugin-main.cpp 側で `obs_module_text()` (`T_()`) を使って渡す。
	 */
	struct DelayTableLabels {
		const char *hdr_ch       = nullptr; ///< "Ch" 列ヘッダー
		const char *hdr_name     = nullptr; ///< "名前" 列ヘッダー
		const char *hdr_measured = nullptr; ///< "計測" 列ヘッダー
		const char *hdr_base     = nullptr; ///< "ベース" 列ヘッダー
		const char *hdr_adjust   = nullptr; ///< "アジャスト" 列ヘッダー
		const char *hdr_global   = nullptr; ///< "共通" 列ヘッダー
		const char *hdr_total    = nullptr; ///< "合計 ms" 列ヘッダー
		const char *lbl_editor   = nullptr; ///< エディタ行ラベル（"アジャスト"）
	};

	obs_property_t *obs_properties_add_delay_table(
		obs_properties_t            *props,
		const char                  *prop_name,
		int                          selected_ch,
		int                          ch_count,
		const DelayTableChannelInfo *channels,
		const DelayTableLabels      &labels); ///< QFormLayout 用プレースホルダーとして OBS_TEXT_INFO プロパティを追加する

	void schedule_delay_table_inject(obs_source_t *source); ///< プレースホルダーを実ウィジェットへ差し替える inject を予約する

} // namespace ods::widgets
