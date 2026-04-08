#pragma once

#include <obs-module.h>

namespace ods::widgets {

	/**
	 * 遅延テーブルウィジェットに渡す 1 チャンネル分の情報。
	 */
	struct DelayTableChannelInfo {
		const char *name;                 ///< メモ名（空文字列可）
		float       measured_ms;          ///< 計測値（片道）。未計測は -1.0f
		int         base_delay_ms;        ///< サブチャンネル基準遅延
		int         offset_ms;            ///< サブチャンネル補正オフセット
		int         master_base_delay_ms; ///< 全サブに加算するマスターベース遅延
		int         master_offset_ms;     ///< 共通オフセット（master_offset_ms）
		int         total_ms;             ///< 合計 = round(measured) + max(0, base + offset + master_base + master_offset)
		bool        warn;                 ///< 生値 total が 0 未満なら true
	};

	/**
	 * テーブル列ヘッダーとエディタラベルの文字列。
	 *
	 * plugin-main.cpp 側で `obs_module_text()` (`T_()`) を使って渡す。
	 */
	struct DelayTableLabels {
		const char *hdr_ch          = nullptr; ///< "Ch" 列ヘッダー
		const char *hdr_name        = nullptr; ///< "名前" 列ヘッダー
		const char *hdr_measured    = nullptr; ///< "計測" 列ヘッダー
		const char *hdr_base        = nullptr; ///< "Sub Base" 列ヘッダー
		const char *hdr_adjust      = nullptr; ///< "Sub Offset" 列ヘッダー
		const char *hdr_master_base = nullptr; ///< "Master Base" 列ヘッダー
		const char *hdr_global      = nullptr; ///< "Master Offset" 列ヘッダー
		const char *hdr_total       = nullptr; ///< "合計 ms" 列ヘッダー
		const char *lbl_editor      = nullptr; ///< エディタ行ラベル（"Sub Offset"）
		const char *grp_sub         = nullptr; ///< グループ見出し（Sub チャンネル値）
		const char *grp_master      = nullptr; ///< グループ見出し（Master 値）
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
