#include "core/constants.hpp"
#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "plugin/plugin-utils.hpp"
#include "ui/properties-builder.hpp"
#include "ui/properties-url-share.hpp"
#include "ui/url-share-renderer.hpp"
#include "widgets/button-bar-widget.hpp"
#include "widgets/url-table-widget.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#define T_(s) obs_module_text(s)

namespace ods::ui::url_share {

	using ods::plugin::DelayStreamData;
	using ods::plugin::TabCtx;
	using ods::tunnel::TunnelState;
	using namespace ods::core;
	using namespace ods::widgets;

	namespace {

		/// サブチャンネル用の共有URLを現在設定から組み立てる。
		std::string make_sub_url(DelayStreamData *d, int ch0) {
			if (!d) return "";
			std::string sid = d->get_stream_id();
			if (sid.empty()) return "";
			std::string code = d->router.sub_code(ch0);
			if (code.empty()) return "";
			std::string base;
			std::string turl = d->tunnel.url();
			if (!turl.empty()) {
				base = turl;
				if (base.rfind("wss://", 0) == 0)
					base.replace(0, 6, "https://");
				else if (base.rfind("ws://", 0) == 0)
					base.replace(0, 5, "http://");
			} else {
				std::string ip = d->get_host_ip();
				if (ip.empty()) ip = d->auto_ip;
				if (ip.empty()) return "";
				int ws_port = d->ws_port.load(std::memory_order_relaxed);
				base        = "http://" + ip + ":" + std::to_string(ws_port);
			}
			if (!base.empty() && base.back() == '/') base.pop_back();
			return base + "/#!/" + sid + "/" + code;
		}

		/// 表示用のサブチャンネルURL一覧を収集する。
		std::vector<UrlShareRow> collect_sub_url_rows(DelayStreamData *d, obs_data_t *s) {
			std::vector<UrlShareRow> rows;
			if (!d || !s) return rows;
			const int sub_count = d->layout.count.load(std::memory_order_relaxed);
			rows.reserve(sub_count);
			for (int di = 0; di < sub_count; ++di) {
				const int   slot     = d->layout.display_order[di];
				const auto  memo_key = ods::plugin::make_sub_memo_key(slot);
				const char *memo     = obs_data_get_string(s, memo_key.data());
				UrlShareRow row;
				row.ch_1indexed = di + 1;
				row.name        = (memo && *memo) ? memo : "";
				row.url         = make_sub_url(d, slot);
				rows.emplace_back(std::move(row));
			}
			return rows;
		}

		// URL 一覧をコピーする共通処理。
		void copy_all_urls(DelayStreamData *d) {
			if (!d) return;
			obs_data_t *s = obs_source_get_settings(d->context);
			if (!s) return;
			auto        rows = collect_sub_url_rows(d, s);
			std::string out  = build_url_share_copy_text(
				rows,
				obs_module_text("NotConfigured"));
			obs_data_release(s);
			if (!out.empty()) ods::plugin::copy_to_clipboard(out);
		}

		/// 全チャンネル分の共有文面をクリップボードへコピーする。
		bool cb_sub_copy_all(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			copy_all_urls(d);
			return false;
		}

		bool cb_copy_and_goto_tab(obs_properties_t *, obs_property_t *, void *priv) {
			auto *ctx = static_cast<TabCtx *>(priv);
			if (!ctx || !ctx->d || !ctx->d->context) return false;
			copy_all_urls(ctx->d);
			ctx->d->set_active_tab(ctx->tab);
			obs_data_t *s = obs_source_get_settings(ctx->d->context);
			if (s) {
				obs_data_set_int(s, "active_tab", ctx->tab);
				obs_data_release(s);
			}
			return true;
		}

	} // namespace

	void add_url_share_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t *grp        = obs_properties_create();
		TunnelState       ts         = d->tunnel.state();
		bool              via_tunnel = !d->tunnel.url().empty();
		const char       *suffix     = (ts == TunnelState::Starting)
										   ? T_("UrlShareStartingSuffix")
										   : (via_tunnel ? T_("UrlShareTunnelSuffix") : T_("UrlShareDirectSuffix"));
		const std::string copy_label = std::string(T_("UrlShareCopyAll")) + suffix;
		obs_property_t   *copy_p =
			obs_properties_add_button2(grp, "url_share_copy_all", copy_label.c_str(), cb_sub_copy_all, d);
		if (ts == TunnelState::Starting) {
			obs_property_set_enabled(copy_p, false);
		}
		{
			obs_data_t *s = obs_source_get_settings(d->context);
			if (s) {
				auto rows = collect_sub_url_rows(d, s);
				obs_data_release(s);

				ods::widgets::UrlTableInfo tbl_info{};
				tbl_info.ch_count = static_cast<int>(rows.size());
				tbl_info.linkify  = (ts != TunnelState::Starting);
				{
					const char *nc = T_("NotConfigured");
					std::strncpy(tbl_info.not_configured, nc ? nc : "-", sizeof(tbl_info.not_configured) - 1);
				}
				const char *reassigning = (ts == TunnelState::Starting) ? T_("UrlShareReassigning") : nullptr;
				for (int i = 0; i < tbl_info.ch_count && i < ods::widgets::UrlTableInfo::kMaxCh; ++i) {
					auto &dst       = tbl_info.rows[i];
					dst.ch_1indexed = rows[i].ch_1indexed;
					std::strncpy(dst.name, rows[i].name.c_str(), sizeof(dst.name) - 1);
					const char *url_src = reassigning ? reassigning : rows[i].url.c_str();
					std::strncpy(dst.url, url_src, sizeof(dst.url) - 1);
				}
				obs_property_t *list_info_p = ods::widgets::obs_properties_add_url_table(
					grp,
					"url_confirm_list_html",
					tbl_info);
				obs_property_text_set_info_word_wrap(list_info_p, false);
			}
		}
		obs_properties_add_group(props, "grp_url_share", T_("GroupUrlShare"), OBS_GROUP_NORMAL, grp);
	}

	void add_url_share_next_button_bar(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		TunnelState ts      = d->tunnel.state();
		const bool  enabled = (ts != TunnelState::Starting);

		const ObsButtonBarSpec copy_next_btn = {
			"url_share_copy_next_act",
			T_("BtnCopyAndNext"),
			cb_copy_and_goto_tab,
			&d->tab_btn_ctx[TAB_SYNC_LATENCY],
			enabled,
		};
		obs_properties_add_button_bar(
			props,
			"url_share_copy_next_bar",
			"",
			nullptr,
			0,
			&copy_next_btn,
			1);
	}

} // namespace ods::ui::url_share
