#include "core/string-format.hpp"
#include "core/constants.hpp"
#include "model/settings-repo.hpp"
#include "plugin/plugin-config.hpp"
#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "plugin/plugin-utils.hpp"
#include "ui/properties-builder.hpp"
#include "ui/properties-channels.hpp"
#include "widgets/button-bar-widget.hpp"
#include "widgets/text-button-widget.hpp"

#include <obs-module.h>
#include <string>
#include <utility>

#define T_(s) obs_module_text(s)

namespace ods::ui::channels {

	using ods::plugin::SubChannelCtx;
	using ods::plugin::TabCtx;
	using ods::plugin::DelayStreamData;
	using namespace ods::core;
	using namespace ods::widgets;

	namespace {

		// 空きスロットを探してサブチャンネルを1つ追加する。
		bool cb_sub_add(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d) return false;
			const Slot vacant = d->layout.find_vacant();
			if (vacant < 0) return false;

			obs_data_t              *s = obs_source_get_settings(d->context);
			ods::model::SettingsRepo repo(s);

			std::string memo = repo.ch_memo(vacant);
			if (memo.empty()) {
				int counter = std::max(repo.memo_auto_counter(), 0);
				memo        = ods::plugin::make_default_sub_memo(counter);
				repo.set_ch_memo(vacant, memo);
				repo.set_memo_auto_counter(counter + 1);
			}
			d->router.set_sub_memo(vacant, memo);

			std::string code = ods::plugin::generate_stream_id(8);
			repo.set_ch_code(vacant, code);
			d->router.set_sub_code(vacant, code);

			d->layout.append(vacant);
			const int next = d->layout.count.load(std::memory_order_relaxed);
			// layout.count が権威ソース — sub_ch_count は obs_data 永続化用のみ

			repo.set_sub_ch_count(next);
			repo.set_ch_display_order(d->layout.serialize());
			obs_data_release(s);

			blog(LOG_INFO, "[obs-delay-stream] cb_sub_add slot=%d count=%d", vacant, next);
			d->router.activate_slot(vacant);
			d->request_props_refresh("cb_sub_add");
			return false;
		}

		// 指定スロットを欠番にする（データ移動なし）。
		bool cb_sub_remove(obs_properties_t *, obs_property_t *, void *priv) {
			auto *ctx = static_cast<SubChannelCtx *>(priv);
			if (!ctx || !ctx->d) return false;
			auto      *d    = ctx->d;
			const Slot slot = ctx->ch;
			if (!d->layout.is_active(slot)) return false;
			if (d->layout.count.load(std::memory_order_relaxed) <= 1) return false;

			d->router.deactivate_slot(slot);
			d->layout.remove(slot);
			const int next = d->layout.count.load(std::memory_order_relaxed);
			// layout.count が権威ソース — sub_ch_count は obs_data 永続化用のみ

			d->delay.channels[slot] = {};
			d->sub_channels[slot].measure.reset();

			obs_data_t              *s = obs_source_get_settings(d->context);
			ods::model::SettingsRepo repo(s);
			repo.clear_channel(slot);
			repo.set_sub_ch_count(next);
			repo.set_ch_display_order(d->layout.serialize());
			obs_data_release(s);

			blog(LOG_INFO, "[obs-delay-stream] cb_sub_remove slot=%d count=%d", slot, next);
			ods::plugin::recalc_all_delays(d);
			d->request_props_refresh("cb_sub_remove");
			return false;
		}

		// 表示順テーブル内で直前と入れ替える（データ移動なし）。
		bool cb_sub_swap_up(obs_properties_t *, obs_property_t *, void *priv) {
			auto *ctx = static_cast<SubChannelCtx *>(priv);
			if (!ctx || !ctx->d) return false;
			auto            *d    = ctx->d;
			const Slot       slot = ctx->ch;
			const DisplayIdx di   = d->layout.display_index(slot);
			if (di <= 0) return false;

			d->layout.swap_display(di - 1, di);

			obs_data_t              *s = obs_source_get_settings(d->context);
			ods::model::SettingsRepo repo(s);
			repo.set_ch_display_order(d->layout.serialize());
			obs_data_release(s);

			blog(LOG_INFO, "[obs-delay-stream] cb_sub_swap_up display %d <-> %d", di, di - 1);
			d->request_props_refresh("cb_sub_swap_up");
			return false;
		}

		// 表示順テーブル内で直後と入れ替える（データ移動なし）。
		bool cb_sub_swap_down(obs_properties_t *, obs_property_t *, void *priv) {
			auto *ctx = static_cast<SubChannelCtx *>(priv);
			if (!ctx || !ctx->d) return false;
			auto            *d    = ctx->d;
			const Slot       slot = ctx->ch;
			const DisplayIdx di   = d->layout.display_index(slot);
			const int        n    = d->layout.count.load(std::memory_order_relaxed);
			if (di < 0 || di >= n - 1) return false;

			d->layout.swap_display(di, di + 1);

			obs_data_t              *s = obs_source_get_settings(d->context);
			ods::model::SettingsRepo repo(s);
			repo.set_ch_display_order(d->layout.serialize());
			obs_data_release(s);

			blog(LOG_INFO, "[obs-delay-stream] cb_sub_swap_down display %d <-> %d", di, di + 1);
			d->request_props_refresh("cb_sub_swap_down");
			return false;
		}

	} // namespace

	void add_sub_channels_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t *grp       = obs_properties_create();
		const int         sub_count = d->layout.count.load(std::memory_order_relaxed);
		for (int di = 0; di < sub_count; ++di) {
			const Slot slot      = d->layout.display_order[di];
			d->sub_btn_ctx[slot] = {d, slot};

			const auto        memo_key = ods::plugin::make_sub_memo_key(slot);
			const std::string lt       = "Ch." + std::to_string(di + 1);

			const auto                    row_prop      = ods::plugin::make_sub_remove_row_key(slot);
			const bool                    can_remove    = (sub_count > 1);
			const bool                    can_up        = (di > 0);
			const bool                    can_down      = (di < sub_count - 1);
			const ObsTextButtonActionSpec row_buttons[] = {
				{T_("SubRemove"), cb_sub_remove, &d->sub_btn_ctx[slot], can_remove},
				{T_("SubMoveUp"), cb_sub_swap_up, &d->sub_btn_ctx[slot], can_up},
				{T_("SubMoveDown"), cb_sub_swap_down, &d->sub_btn_ctx[slot], can_down},
			};
			obs_properties_add_text_buttons(
				grp,
				row_prop.data(),
				lt.c_str(),
				memo_key.data(),
				row_buttons,
				sizeof(row_buttons) / sizeof(row_buttons[0]),
				true,
				SUB_MEMO_MAX_CHARS);
		}
		obs_properties_add_group(props, "grp_sub", T_("GroupSubChannels"), OBS_GROUP_NORMAL, grp);

		// グループ外にチャンネル追加ボタン（左）と「次へ」ボタン（右）を配置する。
		std::string add_label;
		if (sub_count >= MAX_SUB_CH) {
			add_label = T_("SubAddLimitReached");
		} else {
			add_label = string_printf(T_("SubAddFmt"), sub_count + 1);
		}
		const bool add_enabled = (sub_count < MAX_SUB_CH);

		const ObsButtonBarSpec add_btn = {
			"sub_add_act",
			add_label.c_str(),
			cb_sub_add,
			d,
			add_enabled,
		};
		const ObsButtonBarSpec next_btn = {
			"sub_next_tab_act",
			T_("BtnNextWsTab"),
			cb_select_tab,
			&d->tab_btn_ctx[TAB_TUNNEL],
			true,
		};
		obs_properties_add_button_bar(
			props,
			"sub_button_bar",
			"",
			&add_btn,
			1,
			&next_btn,
			1);
	}

} // namespace ods::ui::channels
