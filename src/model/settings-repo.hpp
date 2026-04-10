#pragma once

#include "core/constants.hpp"
#include "model/delay-state.hpp"
#include "plugin/plugin-settings.hpp"

#include <obs-module.h>
#include <string>

namespace ods::model {

	/**
	 * obs_data_t への型安全な読み書きファサード。
	 *
	 * obs_data_get / set 系の散在呼び出しを集約し、
	 * キー名管理と型変換を一元化する。非所有ポインタ。
	 */
	class SettingsRepo {
		obs_data_t *s_;

	public:

		explicit SettingsRepo(obs_data_t *settings) : s_(settings) {}

		obs_data_t *raw() const { return s_; }

		// ============================================================
		// サブチャンネル数
		// ============================================================

		int sub_ch_count() const { return static_cast<int>(obs_data_get_int(s_, "sub_ch_count")); }
		void set_sub_ch_count(int v) { obs_data_set_int(s_, "sub_ch_count", v); }

		int memo_auto_counter() const { return static_cast<int>(obs_data_get_int(s_, "sub_memo_auto_counter")); }
		void set_memo_auto_counter(int v) { obs_data_set_int(s_, "sub_memo_auto_counter", v); }

		// ============================================================
		// チャンネル別フィールド
		// ============================================================

		int ch_measured_ms(int ch) const {
			return static_cast<int>(obs_data_get_int(s_, plugin::make_sub_measured_key(ch).data()));
		}
		void set_ch_measured_ms(int ch, int v) {
			obs_data_set_int(s_, plugin::make_sub_measured_key(ch).data(), v);
		}

		bool ch_ws_measured(int ch) const {
			return obs_data_get_bool(s_, plugin::make_sub_ws_measured_key(ch).data());
		}
		void set_ch_ws_measured(int ch, bool v) {
			obs_data_set_bool(s_, plugin::make_sub_ws_measured_key(ch).data(), v);
		}

		int ch_offset_ms(int ch) const {
			return static_cast<int>(obs_data_get_int(s_, plugin::make_sub_offset_key(ch).data()));
		}
		void set_ch_offset_ms(int ch, int v) {
			obs_data_set_int(s_, plugin::make_sub_offset_key(ch).data(), v);
		}

		std::string ch_memo(int ch) const {
			const char *m = obs_data_get_string(s_, plugin::make_sub_memo_key(ch).data());
			return m ? m : "";
		}
		void set_ch_memo(int ch, const std::string &v) {
			obs_data_set_string(s_, plugin::make_sub_memo_key(ch).data(), v.c_str());
		}

		std::string ch_code(int ch) const {
			const char *c = obs_data_get_string(s_, plugin::make_sub_code_key(ch).data());
			return c ? c : "";
		}
		void set_ch_code(int ch, const std::string &v) {
			obs_data_set_string(s_, plugin::make_sub_code_key(ch).data(), v.c_str());
		}

		// ============================================================
		// チャンネル一括操作
		// ============================================================

		/// ch_from のチャンネル設定を ch_to へコピーする（obs_data のみ）。
		void copy_channel(int ch_from, int ch_to) {
			set_ch_measured_ms(ch_to, ch_measured_ms(ch_from));
			set_ch_ws_measured(ch_to, ch_ws_measured(ch_from));
			set_ch_offset_ms(ch_to, ch_offset_ms(ch_from));
			set_ch_memo(ch_to, ch_memo(ch_from));
			set_ch_code(ch_to, ch_code(ch_from));
		}

		/// from_ch 以降のチャンネル設定を1つ前に詰める（削除操作用）。
		void shift_channels_down(int from_ch) {
			for (int i = from_ch; i < core::MAX_SUB_CH - 1; ++i) {
				copy_channel(i + 1, i);
			}
			clear_channel(core::MAX_SUB_CH - 1);
		}

		/// ch_a と ch_b のチャンネル設定を入れ替える。
		void swap_channels(int ch_a, int ch_b) {
			const int         a_measured    = ch_measured_ms(ch_a);
			const int         b_measured    = ch_measured_ms(ch_b);
			const bool        a_ws_measured = ch_ws_measured(ch_a);
			const bool        b_ws_measured = ch_ws_measured(ch_b);
			const int         a_offset      = ch_offset_ms(ch_a);
			const int         b_offset      = ch_offset_ms(ch_b);
			const std::string a_memo        = ch_memo(ch_a);
			const std::string b_memo        = ch_memo(ch_b);
			const std::string a_code        = ch_code(ch_a);
			const std::string b_code        = ch_code(ch_b);

			set_ch_measured_ms(ch_a, b_measured);
			set_ch_measured_ms(ch_b, a_measured);
			set_ch_ws_measured(ch_a, b_ws_measured);
			set_ch_ws_measured(ch_b, a_ws_measured);
			set_ch_offset_ms(ch_a, b_offset);
			set_ch_offset_ms(ch_b, a_offset);
			set_ch_memo(ch_a, b_memo);
			set_ch_memo(ch_b, a_memo);
			set_ch_code(ch_a, b_code);
			set_ch_code(ch_b, a_code);
		}

		/// 指定チャンネルの全設定をゼロ/空にする。
		void clear_channel(int ch) {
			set_ch_measured_ms(ch, 0);
			set_ch_ws_measured(ch, false);
			set_ch_offset_ms(ch, 0);
			set_ch_memo(ch, "");
			set_ch_code(ch, "");
		}

		// ============================================================
		// 計測結果（グローバル）
		// ============================================================

		int measured_rtsp_e2e_ms() const {
			return static_cast<int>(obs_data_get_int(s_, plugin::kMeasuredRtspE2eKey));
		}
		void set_measured_rtsp_e2e_ms(int v) {
			obs_data_set_int(s_, plugin::kMeasuredRtspE2eKey, v);
		}

		bool rtsp_e2e_measured() const {
			return obs_data_get_bool(s_, plugin::kRtspE2eMeasuredKey);
		}
		void set_rtsp_e2e_measured(bool v) {
			obs_data_set_bool(s_, plugin::kRtspE2eMeasuredKey, v);
		}
	};

} // namespace ods::model
