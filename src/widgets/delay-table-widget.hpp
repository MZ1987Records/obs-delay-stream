#pragma once

#include <obs-module.h>

namespace ods::widgets {

	/**
	 * 遅延テーブルウィジェットに渡す 1 チャンネル分の情報。
	 */
	struct DelayTableChannelInfo {
		const char *name;         ///< メモ名（空文字列可）
		float       measured_ms;  ///< 計測値（片道）。未計測は -1.0f
		int         offset_ms;    ///< チャンネル別補正オフセット
		int         raw_delay_ms; ///< R - A - C[i] - B + offset[i]（負値許容）
		int         neg_max_ms;   ///< 負値フロア補正量（全チャンネル共通）
		int         total_ms;     ///< raw_delay_ms + neg_max_ms（最終適用値）
		bool        warn;         ///< raw_delay_ms < 0 なら true
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
		const char *hdr_adjust   = nullptr; ///< "Offset" 列ヘッダー
		const char *hdr_raw      = nullptr; ///< "計算値" 列ヘッダー
		const char *hdr_floor    = nullptr; ///< "補正" 列ヘッダー
		const char *hdr_total    = nullptr; ///< "合計 ms" 列ヘッダー
		const char *lbl_editor   = nullptr; ///< エディタ行ラベル（"Offset"）
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
