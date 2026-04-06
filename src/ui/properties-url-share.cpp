#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "plugin/plugin-utils.hpp"
#include "ui/properties-builder.hpp"
#include "ui/url-share-renderer.hpp"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <cmath>
#include <string>
#include <vector>

#define T_(s) obs_module_text(s)

namespace ods::ui {

using ods::plugin::DelayStreamData;
using ods::tunnel::TunnelState;

namespace {

QColor pick_alt_row_color(const QColor &base, const QColor &alt_candidate) {
	const double diff = std::abs(base.lightnessF() - alt_candidate.lightnessF());
	if (diff > 0.18) return base;
	return alt_candidate;
}

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

std::vector<UrlShareRow>
collect_sub_url_rows(DelayStreamData *d, obs_data_t *s) {
	std::vector<UrlShareRow> rows;
	if (!d || !s) return rows;
	int sub_count = d->sub_ch_count;
	rows.reserve(sub_count);
	for (int i = 0; i < sub_count; ++i) {
		const auto  memo_key = ods::plugin::make_sub_memo_key(i);
		const char *memo     = obs_data_get_string(s, memo_key.data());
		UrlShareRow row;
		row.ch_1indexed = i + 1;
		row.name        = (memo && *memo) ? memo : "";
		row.url         = make_sub_url(d, i);
		rows.emplace_back(std::move(row));
	}
	return rows;
}

} // namespace

bool PropertiesBuilder::cb_sub_copy_all(obs_properties_t *, obs_property_t *, void *priv) {
	auto *d = static_cast<DelayStreamData *>(priv);
	if (!d) return false;
	obs_data_t *s = obs_source_get_settings(d->context);
	if (!s) return false;
	auto        rows = collect_sub_url_rows(d, s);
	std::string out  = build_url_share_copy_text(
		rows,
		T_("NotConfigured"));
	obs_data_release(s);
	if (!out.empty()) ods::plugin::copy_to_clipboard(out);
	return false;
}

void PropertiesBuilder::add_url_share_group() {
	if (!props_ || !d_) return;
	obs_properties_t *grp        = obs_properties_create();
	TunnelState       ts         = d_->tunnel.state();
	bool              via_tunnel = !d_->tunnel.url().empty();
	{
		obs_data_t *s = obs_source_get_settings(d_->context);
		if (s) {
			auto           rows          = collect_sub_url_rows(d_, s);
			const QPalette pal           = QApplication::palette();
			const QColor   base_color    = pal.color(QPalette::Base);
			const QColor   alt_row_color = pick_alt_row_color(
				base_color,
				pal.color(QPalette::AlternateBase));
			UrlConfirmThemeColors theme_colors;
			theme_colors.table_bg =
				pal.color(QPalette::Window).name(QColor::HexRgb).toStdString();
			theme_colors.header_bg =
				pal.color(QPalette::Button).name(QColor::HexRgb).toStdString();
			theme_colors.header_text =
				pal.color(QPalette::ButtonText).name(QColor::HexRgb).toStdString();
			theme_colors.row_bg =
				base_color.name(QColor::HexRgb).toStdString();
			theme_colors.alt_row_bg =
				alt_row_color.name(QColor::HexRgb).toStdString();
			theme_colors.text =
				pal.color(QPalette::Text).name(QColor::HexRgb).toStdString();
			theme_colors.border =
				pal.color(QPalette::Mid).name(QColor::HexRgb).toStdString();
			theme_colors.link =
				pal.color(QPalette::Link).name(QColor::HexRgb).toStdString();
			const bool  linkify_urls = (ts != TunnelState::Starting);
			std::string list_html    = build_url_confirm_html_text(
				rows,
				T_("NotConfigured"),
				&theme_colors,
				linkify_urls);
			obs_data_release(s);
			obs_property_t *list_info_p = obs_properties_add_text(
				grp,
				"url_confirm_list_html",
				list_html.c_str(),
				OBS_TEXT_INFO);
			obs_property_text_set_info_word_wrap(list_info_p, false);
		}
	}
	const char *suffix = (ts == TunnelState::Starting)
							 ? T_("UrlShareStartingSuffix")
							 : (via_tunnel ? T_("UrlShareTunnelSuffix") : T_("UrlShareDirectSuffix"));
	char        copy_label[192];
	snprintf(copy_label, sizeof(copy_label), "%s%s", T_("UrlShareCopyAll"), suffix);
	obs_property_t *copy_p =
		obs_properties_add_button2(grp, "url_share_copy_all", copy_label, cb_sub_copy_all, d_);
	if (ts == TunnelState::Starting) {
		obs_property_set_enabled(copy_p, false);
	}
	obs_properties_add_group(props_, "grp_url_share", T_("GroupUrlShare"), OBS_GROUP_NORMAL, grp);
}

} // namespace ods::ui
