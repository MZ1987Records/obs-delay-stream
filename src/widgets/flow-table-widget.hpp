#pragma once

#include <obs-module.h>

namespace ods::widgets {

	/**
	 * WS 計測テーブルウィジェットに渡す 1 チャンネル分の情報。
	 */
	struct FlowTableChannelInfo {
		const char *name;        ///< メモ名（空文字列可）
		bool        connected;   ///< 接続中か
		int         measured_ms; ///< 計測結果 ms。未計測は -1、失敗は -2
	};

	/**
	 * テーブル列ヘッダーとステータス文字列。
	 */
	struct FlowTableLabels {
		const char *hdr_ch;              ///< "Ch" 列ヘッダー
		const char *hdr_name;            ///< "名前" 列ヘッダー
		const char *hdr_status;          ///< "状態" 列ヘッダー
		const char *hdr_result;          ///< "計測結果" 列ヘッダー
		const char *status_connected;    ///< "接続中"
		const char *status_disconnected; ///< "未接続"
		const char *result_failed;       ///< "(失敗)"
	};

	obs_property_t *obs_properties_add_flow_table(
		obs_properties_t           *props,
		const char                 *prop_name,
		int                         ch_count,
		const FlowTableChannelInfo *channels,
		const FlowTableLabels      &labels); ///< QFormLayout 用プレースホルダーとして OBS_TEXT_INFO プロパティを追加する

	void schedule_flow_table_inject(obs_source_t *source); ///< プレースホルダーを実ウィジェットへ差し替える inject を予約する

} // namespace ods::widgets
