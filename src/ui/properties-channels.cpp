#include "core/string-format.hpp"
#include "core/constants.hpp"
#include "model/settings-repo.hpp"
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
			ods::model::SettingsRepo repo(s);

			std::string memo = repo.ch_memo(added_ch);
			if (memo.empty()) {
				int counter = std::max(repo.memo_auto_counter(), 0);
				memo = ods::plugin::make_default_sub_memo(counter);
				repo.set_ch_memo(added_ch, memo);
				repo.set_memo_auto_counter(counter + 1);
			}
			d->router.set_sub_memo(added_ch, memo);

			std::string code = ods::plugin::generate_stream_id(8);
			repo.set_ch_code(added_ch, code);
			d->router.set_sub_code(added_ch, code);

			repo.set_sub_ch_count(next);
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
			ods::model::SettingsRepo repo(s);
			repo.shift_channels_down(ch);
			for (int i = ch; i < MAX_SUB_CH - 1; ++i) {
				d->router.set_sub_memo(i, repo.ch_memo(i));
				d->router.set_sub_code(i, repo.ch_code(i));
			}
			d->router.set_sub_code(MAX_SUB_CH - 1, "");
			repo.set_sub_ch_count(next);
			obs_data_release(s);
			blog(LOG_INFO, "[obs-delay-stream] cb_sub_remove sub_ch_count %d -> %d (remove ch=%d)", cur, next, ch + 1);

			for (int i = ch; i < MAX_SUB_CH - 1; ++i) {
				d->delay.channels[i] = d->delay.channels[i + 1];
				d->sub_channels[i].measure.reset();
			}
			d->delay.channels[MAX_SUB_CH - 1] = {};
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
			ods::model::SettingsRepo repo(s);
			repo.swap_channels(prev, ch);
			d->router.set_sub_memo(prev, repo.ch_memo(prev));
			d->router.set_sub_memo(ch, repo.ch_memo(ch));
			d->router.set_sub_code(prev, repo.ch_code(prev));
			d->router.set_sub_code(ch, repo.ch_code(ch));
			obs_data_release(s);

			std::swap(d->delay.channels[prev], d->delay.channels[ch]);
			ods::plugin::recalc_all_delays(d);
			d->sub_channels[prev].measure.reset();
			d->sub_channels[ch].measure.reset();

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
