#include "core/string-format.hpp"
#include "core/constants.hpp"
#include "plugin/plugin-config.hpp"
#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "plugin/plugin-utils.hpp"
#include "ui/properties-channels.hpp"
#include "widgets/text-button-widget.hpp"

#include <obs-module.h>
#include <string>
#include <utility>

#define T_(s) obs_module_text(s)

namespace ods::ui::channels {

	using ods::plugin::SubChannelCtx;
	using ods::plugin::DelayStreamData;
	using namespace ods::core;
	using namespace ods::widgets;

	namespace {

		// サブチャンネルを1つ追加し、初期メモ/コードを設定する。
		bool cb_sub_add(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d) return false;
			if (d->router_running.load()) return false;
			int cur = d->delay.sub_ch_count;
			if (cur >= MAX_SUB_CH) return false;
			int         next     = ods::plugin::clamp_sub_ch_count(cur + 1);
			int         added_ch = next - 1;
			obs_data_t *s        = obs_source_get_settings(d->context);

			const auto  memo_key = ods::plugin::make_sub_memo_key(added_ch);
			const char *memo     = obs_data_get_string(s, memo_key.data());
			if (!memo || !*memo) {
				int counter = (int)obs_data_get_int(s, "sub_memo_auto_counter");
				if (counter < 0) counter = 0;
				std::string auto_memo = ods::plugin::make_default_sub_memo(counter);
				obs_data_set_string(s, memo_key.data(), auto_memo.c_str());
				obs_data_set_int(s, "sub_memo_auto_counter", counter + 1);
				d->router.set_sub_memo(added_ch, auto_memo);
			} else {
				d->router.set_sub_memo(added_ch, memo);
			}
			{
				const auto  code_key = ods::plugin::make_sub_code_key(added_ch);
				std::string code     = ods::plugin::generate_stream_id(8);
				obs_data_set_string(s, code_key.data(), code.c_str());
				d->router.set_sub_code(added_ch, code);
			}

			obs_data_set_int(s, "sub_ch_count", next);
			obs_data_release(s);
			blog(LOG_INFO, "[obs-delay-stream] cb_sub_add sub_ch_count %d -> %d", cur, next);
			d->delay.sub_ch_count = next;
			d->router.set_active_channels(next);
			d->flow.set_active_channels(next);
			d->flow.reset();
			d->request_props_refresh("cb_sub_add");
			return false;
		}

		// 指定サブチャンネルを削除し、後続チャンネル設定を前詰めする。
		bool cb_sub_remove(obs_properties_t *, obs_property_t *, void *priv) {
			auto *ctx = static_cast<SubChannelCtx *>(priv);
			if (!ctx || !ctx->d) return false;
			auto *d = ctx->d;
			if (d->router_running.load()) return false;
			int cur = d->delay.sub_ch_count;
			if (cur <= 1) return false;
			int ch = ctx->ch;
			if (ch < 0 || ch >= cur) return false;
			int         next = ods::plugin::clamp_sub_ch_count(cur - 1);
			obs_data_t *s    = obs_source_get_settings(d->context);
			for (int i = ch; i < MAX_SUB_CH - 1; ++i) {
				const auto measured_from = ods::plugin::make_sub_measured_key(i + 1);
				const auto measured_to   = ods::plugin::make_sub_measured_key(i);
				int        v             = static_cast<int>(obs_data_get_int(s, measured_from.data()));
				obs_data_set_int(s, measured_to.data(), v);

				const auto ws_measured_from = ods::plugin::make_sub_ws_measured_key(i + 1);
				const auto ws_measured_to   = ods::plugin::make_sub_ws_measured_key(i);
				obs_data_set_bool(s, ws_measured_to.data(), obs_data_get_bool(s, ws_measured_from.data()));

				const auto offset_from = ods::plugin::make_sub_offset_key(i + 1);
				const auto offset_to   = ods::plugin::make_sub_offset_key(i);
				int        ov          = static_cast<int>(obs_data_get_int(s, offset_from.data()));
				obs_data_set_int(s, offset_to.data(), ov);

				const auto  memo_from = ods::plugin::make_sub_memo_key(i + 1);
				const auto  memo_to   = ods::plugin::make_sub_memo_key(i);
				const char *m         = obs_data_get_string(s, memo_from.data());
				obs_data_set_string(s, memo_to.data(), m ? m : "");
				d->router.set_sub_memo(i, m ? m : "");

				const auto  code_from = ods::plugin::make_sub_code_key(i + 1);
				const auto  code_to   = ods::plugin::make_sub_code_key(i);
				const char *c         = obs_data_get_string(s, code_from.data());
				obs_data_set_string(s, code_to.data(), c ? c : "");
				d->router.set_sub_code(i, c ? c : "");
			}
			{
				const auto measured_last = ods::plugin::make_sub_measured_key(MAX_SUB_CH - 1);
				obs_data_set_int(s, measured_last.data(), 0);
				const auto ws_measured_last = ods::plugin::make_sub_ws_measured_key(MAX_SUB_CH - 1);
				obs_data_set_bool(s, ws_measured_last.data(), false);
				const auto offset_last = ods::plugin::make_sub_offset_key(MAX_SUB_CH - 1);
				obs_data_set_int(s, offset_last.data(), 0);
				const auto code_last = ods::plugin::make_sub_code_key(MAX_SUB_CH - 1);
				obs_data_set_string(s, code_last.data(), "");
				d->router.set_sub_code(MAX_SUB_CH - 1, "");
			}
			obs_data_set_int(s, "sub_ch_count", next);
			obs_data_release(s);
			blog(LOG_INFO, "[obs-delay-stream] cb_sub_remove sub_ch_count %d -> %d (remove ch=%d)", cur, next, ch + 1);

			for (int i = ch; i < MAX_SUB_CH - 1; ++i) {
				d->delay.channels[i].measured_ms = d->delay.channels[i + 1].measured_ms;
				d->delay.channels[i].ws_measured = d->delay.channels[i + 1].ws_measured;
				d->delay.channels[i].offset_ms   = d->delay.channels[i + 1].offset_ms;
				d->sub_channels[i].measure.reset();
			}
			d->delay.channels[MAX_SUB_CH - 1].measured_ms = 0;
			d->delay.channels[MAX_SUB_CH - 1].ws_measured = false;
			d->delay.channels[MAX_SUB_CH - 1].offset_ms   = 0;
			d->sub_channels[MAX_SUB_CH - 1].measure.reset();
			ods::plugin::recalc_all_delays(d);

			d->delay.sub_ch_count = next;
			d->router.set_active_channels(next);
			d->flow.set_active_channels(next);
			d->flow.reset();
			d->request_props_refresh("cb_sub_remove");
			return false;
		}

		// 指定サブチャンネルと直前サブチャンネルの内容を入れ替える。
		bool cb_sub_swap_up(obs_properties_t *, obs_property_t *, void *priv) {
			auto *ctx = static_cast<SubChannelCtx *>(priv);
			if (!ctx || !ctx->d) return false;
			auto *d = ctx->d;
			if (d->router_running.load()) return false;
			int cur = d->delay.sub_ch_count;
			int ch  = ctx->ch;
			if (ch <= 0 || ch >= cur) return false;
			int prev = ch - 1;

			obs_data_t *s = obs_source_get_settings(d->context);
			if (!s) return false;
			const auto prev_measured_key = ods::plugin::make_sub_measured_key(prev);
			const auto ch_measured_key   = ods::plugin::make_sub_measured_key(ch);
			const auto prev_ws_meas_key  = ods::plugin::make_sub_ws_measured_key(prev);
			const auto ch_ws_meas_key    = ods::plugin::make_sub_ws_measured_key(ch);
			const auto prev_adj_key      = ods::plugin::make_sub_offset_key(prev);
			const auto ch_adj_key        = ods::plugin::make_sub_offset_key(ch);
			const auto prev_memo_key     = ods::plugin::make_sub_memo_key(prev);
			const auto ch_memo_key       = ods::plugin::make_sub_memo_key(ch);
			const auto prev_code_key     = ods::plugin::make_sub_code_key(prev);
			const auto ch_code_key       = ods::plugin::make_sub_code_key(ch);

			const int         prev_measured    = static_cast<int>(obs_data_get_int(s, prev_measured_key.data()));
			const int         ch_measured      = static_cast<int>(obs_data_get_int(s, ch_measured_key.data()));
			const bool        prev_ws_measured = obs_data_get_bool(s, prev_ws_meas_key.data());
			const bool        ch_ws_measured   = obs_data_get_bool(s, ch_ws_meas_key.data());
			const int         prev_adj         = static_cast<int>(obs_data_get_int(s, prev_adj_key.data()));
			const int         ch_adj           = static_cast<int>(obs_data_get_int(s, ch_adj_key.data()));
			const char       *prev_memo_raw    = obs_data_get_string(s, prev_memo_key.data());
			const char       *ch_memo_raw      = obs_data_get_string(s, ch_memo_key.data());
			const char       *prev_code_raw    = obs_data_get_string(s, prev_code_key.data());
			const char       *ch_code_raw      = obs_data_get_string(s, ch_code_key.data());
			const std::string prev_memo        = prev_memo_raw ? prev_memo_raw : "";
			const std::string ch_memo          = ch_memo_raw ? ch_memo_raw : "";
			const std::string prev_code        = prev_code_raw ? prev_code_raw : "";
			const std::string ch_code          = ch_code_raw ? ch_code_raw : "";

			obs_data_set_int(s, prev_measured_key.data(), ch_measured);
			obs_data_set_int(s, ch_measured_key.data(), prev_measured);
			obs_data_set_bool(s, prev_ws_meas_key.data(), ch_ws_measured);
			obs_data_set_bool(s, ch_ws_meas_key.data(), prev_ws_measured);
			obs_data_set_int(s, prev_adj_key.data(), ch_adj);
			obs_data_set_int(s, ch_adj_key.data(), prev_adj);
			obs_data_set_string(s, prev_memo_key.data(), ch_memo.c_str());
			obs_data_set_string(s, ch_memo_key.data(), prev_memo.c_str());
			obs_data_set_string(s, prev_code_key.data(), ch_code.c_str());
			obs_data_set_string(s, ch_code_key.data(), prev_code.c_str());
			obs_data_release(s);

			std::swap(d->delay.channels[prev].measured_ms, d->delay.channels[ch].measured_ms);
			std::swap(d->delay.channels[prev].ws_measured, d->delay.channels[ch].ws_measured);
			std::swap(d->delay.channels[prev].offset_ms, d->delay.channels[ch].offset_ms);
			ods::plugin::recalc_all_delays(d);
			d->sub_channels[prev].measure.reset();
			d->sub_channels[ch].measure.reset();
			d->router.set_sub_memo(prev, ch_memo);
			d->router.set_sub_memo(ch, prev_memo);
			d->router.set_sub_code(prev, ch_code);
			d->router.set_sub_code(ch, prev_code);

			blog(LOG_INFO, "[obs-delay-stream] cb_sub_swap_up ch=%d <-> ch=%d", ch + 1, prev + 1);
			d->flow.reset();
			d->request_props_refresh("cb_sub_swap_up");
			return false;
		}

		// 指定サブチャンネルと直後サブチャンネルの内容を入れ替える。
		bool cb_sub_swap_down(obs_properties_t *, obs_property_t *, void *priv) {
			auto *ctx = static_cast<SubChannelCtx *>(priv);
			if (!ctx || !ctx->d) return false;
			auto *d = ctx->d;
			if (d->router_running.load()) return false;
			int ch  = ctx->ch;
			int cur = d->delay.sub_ch_count;
			if (ch < 0 || ch >= cur - 1) return false;
			SubChannelCtx next_ctx{d, ch + 1};
			return cb_sub_swap_up(nullptr, nullptr, &next_ctx);
		}

	} // namespace

	void add_sub_channels_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t *grp       = obs_properties_create();
		int               sub_count = d->delay.sub_ch_count;
		for (int i = 0; i < sub_count; ++i) {
			d->sub_btn_ctx[i] = {d, i};

			const auto        memo_key = ods::plugin::make_sub_memo_key(i);
			const std::string lt       = "Ch." + std::to_string(i + 1);

			const auto                    row_prop      = ods::plugin::make_sub_remove_row_key(i);
			const bool                    input_enabled = !d->router_running.load();
			const bool                    can_remove    = !(d->router_running.load() || sub_count <= 1);
			const bool                    can_up        = !(d->router_running.load() || i <= 0);
			const bool                    can_down      = !(d->router_running.load() || i >= (sub_count - 1));
			const ObsTextButtonActionSpec row_buttons[] = {
				{T_("SubRemove"), cb_sub_remove, &d->sub_btn_ctx[i], can_remove},
				{T_("SubMoveUp"), cb_sub_swap_up, &d->sub_btn_ctx[i], can_up},
				{T_("SubMoveDown"), cb_sub_swap_down, &d->sub_btn_ctx[i], can_down},
			};
			obs_properties_add_text_buttons(
				grp,
				row_prop.data(),
				lt.c_str(),
				memo_key.data(),
				row_buttons,
				sizeof(row_buttons) / sizeof(row_buttons[0]),
				input_enabled,
				SUB_MEMO_MAX_CHARS);
		}
		obs_property_t *spc_bottom = obs_properties_add_text(grp, "sub_add_spacer", "", OBS_TEXT_INFO);
		obs_property_set_long_description(spc_bottom, " ");
		obs_property_text_set_info_word_wrap(spc_bottom, false);
		std::string add_label;
		if (d->delay.sub_ch_count >= MAX_SUB_CH) {
			add_label = T_("SubAddLimitReached");
		} else {
			add_label = string_printf(T_("SubAddFmt"), d->delay.sub_ch_count + 1);
		}
		obs_property_t *add_p =
			obs_properties_add_button2(grp, "sub_add_btn", add_label.c_str(), cb_sub_add, d);
		if (d->router_running.load() || d->delay.sub_ch_count >= MAX_SUB_CH) {
			obs_property_set_enabled(add_p, false);
		}
		obs_properties_add_group(props, "grp_sub", T_("GroupSubChannels"), OBS_GROUP_NORMAL, grp);
	}

} // namespace ods::ui::channels
