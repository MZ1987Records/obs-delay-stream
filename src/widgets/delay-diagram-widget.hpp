#pragma once

#include <obs-module.h>

namespace ods::widgets {

	/**
	 * ディレイタイミング図ウィジェットに渡す描画データ。
	 *
	 * DelayViewModel から必要なフィールドだけを抽出し、
	 * QPainter によるタイミング図描画に必要な値をまとめる。
	 */
	struct DelayDiagramInfo {
		int R;            ///< OBS 配信レイテンシ (ms)
		int A;            ///< アバターレイテンシ (ms)
		int buf;          ///< 再生バッファ (ms)
		int ch_count;     ///< チャンネル数
		int master_delay; ///< OBS 出力ディレイ = neg_max (ms)

		struct ChInfo {
			float measured_ms; ///< ブラウザ配信レイテンシ C[i] (ms)。未計測は -1.0f
			int   total_ms;    ///< チャンネルディレイ (ms)
			int   offset_ms;   ///< チャンネル補正オフセット (ms)
		};
		static constexpr int kMaxCh = 20;
		ChInfo               channels[kMaxCh]{};
	};

	/**
	 * タイミング図の凡例ラベル文字列。
	 *
	 * plugin-main.cpp 側で obs_module_text() を使って渡す。
	 */
	struct DelayDiagramLabels {
		const char *legend_delay      = nullptr; ///< "自動調整ディレイ"
		const char *legend_delay_desc = nullptr; ///< "は上記の値に基づいて…"
		const char *legend_ws         = nullptr; ///< "ブラウザ配信レイテンシ"
		const char *legend_env        = nullptr; ///< "想定環境レイテンシ"
		const char *legend_buf        = nullptr; ///< "再生バッファ"
		const char *legend_avatar     = nullptr; ///< "アバターレイテンシ"
		const char *legend_broadcast  = nullptr; ///< "OBS配信レイテンシ"
		const char *lane_broadcast    = nullptr; ///< "配信" (レーンラベル)
		const char *no_data           = nullptr; ///< "計測データなし"
	};

	/// QFormLayout 用プレースホルダーとして OBS_TEXT_INFO プロパティを追加する
	obs_property_t *obs_properties_add_delay_diagram(
		obs_properties_t         *props,
		const char               *prop_name,
		const DelayDiagramInfo   &info,
		const DelayDiagramLabels &labels);

	/// プレースホルダーを実ウィジェットへ差し替える inject を予約する
	void schedule_delay_diagram_inject(obs_source_t *source);

} // namespace ods::widgets
