#include "plugin-main-url-share-ui.hpp"

#include <string>
#include <vector>

#include "plugin-main-state.hpp"
#include "plugin-main-sub-settings.hpp"
#include "plugin-main-url-share-renderer.hpp"
#include "plugin-main-utils.hpp"

#define T_(s) obs_module_text(s)

namespace plugin_main_url_share_ui {

namespace {

std::string make_sub_url(DelayStreamData* d, int ch0) {
    if (!d) return "";
    std::string sid = d->get_stream_id();
    if (sid.empty()) return "";
    std::string code = d->router.sub_code(ch0);
    if (code.empty()) return "";
    std::string base;
    std::string turl = d->tunnel.url();
    if (!turl.empty()) {
        base = turl;
        if (base.rfind("wss://", 0) == 0) base.replace(0, 6, "https://");
        else if (base.rfind("ws://", 0) == 0) base.replace(0, 5, "http://");
    } else {
        std::string ip = d->get_host_ip();
        if (ip.empty()) ip = d->auto_ip;
        if (ip.empty()) return "";
        int ws_port = d->ws_port.load(std::memory_order_relaxed);
        base = "http://" + ip + ":" + std::to_string(ws_port);
    }
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/#!/" + sid + "/" + code;
}

std::vector<plugin_main_url_share_renderer::UrlShareRow>
collect_sub_url_rows(DelayStreamData* d, obs_data_t* s) {
    std::vector<plugin_main_url_share_renderer::UrlShareRow> rows;
    if (!d || !s) return rows;
    int sub_count = d->sub_ch_count;
    rows.reserve(sub_count);
    for (int i = 0; i < sub_count; ++i) {
        const auto memo_key = plugin_main_sub_settings::make_sub_memo_key(i);
        const char* memo = obs_data_get_string(s, memo_key.data());
        plugin_main_url_share_renderer::UrlShareRow row;
        row.ch_1indexed = i + 1;
        row.name = (memo && *memo) ? memo : "";
        row.url = make_sub_url(d, i);
        rows.emplace_back(std::move(row));
    }
    return rows;
}

bool cb_sub_copy_all(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    obs_data_t* s = obs_source_get_settings(d->context);
    if (!s) return false;
    auto rows = collect_sub_url_rows(d, s);
    std::string out = plugin_main_url_share_renderer::build_url_share_copy_text(
        rows, T_("NotConfigured"));
    obs_data_release(s);
    if (!out.empty()) plugin_main_utils::copy_to_clipboard(out);
    return false;
}

} // namespace

void add_url_share_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    TunnelState ts = d->tunnel.state();
    bool via_tunnel = !d->tunnel.url().empty();
    {
        obs_data_t* s = obs_source_get_settings(d->context);
        if (s) {
            auto rows = collect_sub_url_rows(d, s);
            std::string list_html = plugin_main_url_share_renderer::build_url_confirm_html_text(
                rows, T_("NotConfigured"));
            obs_data_release(s);
            obs_property_t* list_info_p = obs_properties_add_text(
                grp, "url_confirm_list_html", list_html.c_str(), OBS_TEXT_INFO);
            obs_property_text_set_info_word_wrap(list_info_p, false);
        }
    }
    const char* suffix = via_tunnel
        ? T_("UrlShareTunnelSuffix")
        : T_("UrlShareDirectSuffix");
    char copy_label[192];
    snprintf(copy_label, sizeof(copy_label), "%s%s", T_("UrlShareCopyAll"), suffix);
    obs_property_t* copy_p =
        obs_properties_add_button2(grp, "url_share_copy_all", copy_label, cb_sub_copy_all, d);
    if (ts == TunnelState::Starting) {
        obs_property_set_enabled(copy_p, false);
    }
    obs_properties_add_group(props, "grp_url_share", T_("GroupUrlShare"), OBS_GROUP_NORMAL, grp);
}

} // namespace plugin_main_url_share_ui
