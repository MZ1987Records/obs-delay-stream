#include "plugin-main-properties-ui.hpp"

#include <cstdio>
#include <string>

#include <obs-module.h>

#include "constants.hpp"
#include "plugin-main-audio-processing.hpp"
#include "plugin-main-config.hpp"
#include "plugin-main-state.hpp"
#include "plugin-main-sub-settings.hpp"
#include "plugin-main-utils.hpp"
#include "text-button-widget.hpp"

#define T_(s) obs_module_text(s)

namespace plugin_main_properties_ui {

bool PropertiesBuilder::cb_sub_add(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    if (d->router_running.load()) return false;
    int cur = d->sub_ch_count;
    if (cur >= MAX_SUB_CH) return false;
    int next = plugin_main_utils::clamp_sub_ch_count(cur + 1);
    int added_ch = next - 1;
    obs_data_t* s = obs_source_get_settings(d->context);

    const auto memo_key = plugin_main_sub_settings::make_sub_memo_key(added_ch);
    const char* memo = obs_data_get_string(s, memo_key.data());
    if (!memo || !*memo) {
        int counter = (int)obs_data_get_int(s, "sub_memo_auto_counter");
        if (counter < 0) counter = 0;
        std::string auto_memo = plugin_main_config::make_default_sub_memo(counter);
        obs_data_set_string(s, memo_key.data(), auto_memo.c_str());
        obs_data_set_int(s, "sub_memo_auto_counter", counter + 1);
        d->router.set_sub_memo(added_ch, auto_memo);
    } else {
        d->router.set_sub_memo(added_ch, memo);
    }
    {
        const auto code_key = plugin_main_sub_settings::make_sub_code_key(added_ch);
        std::string code = plugin_main_utils::generate_stream_id(8);
        obs_data_set_string(s, code_key.data(), code.c_str());
        d->router.set_sub_code(added_ch, code);
    }

    obs_data_set_int(s, "sub_ch_count", next);
    obs_data_release(s);
    blog(LOG_INFO, "[obs-delay-stream] cb_sub_add sub_ch_count %d -> %d", cur, next);
    d->sub_ch_count = next;
    d->router.set_active_channels(next);
    d->flow.set_active_channels(next);
    d->flow.reset();
    d->request_props_refresh("cb_sub_add");
    return false;
}

bool PropertiesBuilder::cb_sub_remove(obs_properties_t*, obs_property_t*, void* priv) {
    auto* ctx = static_cast<ChCtx*>(priv);
    if (!ctx || !ctx->d) return false;
    auto* d = ctx->d;
    if (d->router_running.load()) return false;
    int cur = d->sub_ch_count;
    if (cur <= 1) return false;
    int ch = ctx->ch;
    if (ch < 0 || ch >= cur) return false;
    int next = plugin_main_utils::clamp_sub_ch_count(cur - 1);
    obs_data_t* s = obs_source_get_settings(d->context);
    for (int i = ch; i < MAX_SUB_CH - 1; ++i) {
        const auto delay_from = plugin_main_sub_settings::make_sub_delay_key(i + 1);
        const auto delay_to = plugin_main_sub_settings::make_sub_delay_key(i);
        double v = obs_data_get_double(s, delay_from.data());
        obs_data_set_double(s, delay_to.data(), v);

        const auto adjust_from = plugin_main_sub_settings::make_sub_adjust_key(i + 1);
        const auto adjust_to = plugin_main_sub_settings::make_sub_adjust_key(i);
        double av = obs_data_get_double(s, adjust_from.data());
        obs_data_set_double(s, adjust_to.data(), av);

        const auto memo_from = plugin_main_sub_settings::make_sub_memo_key(i + 1);
        const auto memo_to = plugin_main_sub_settings::make_sub_memo_key(i);
        const char* m = obs_data_get_string(s, memo_from.data());
        obs_data_set_string(s, memo_to.data(), m ? m : "");
        d->router.set_sub_memo(i, m ? m : "");

        const auto code_from = plugin_main_sub_settings::make_sub_code_key(i + 1);
        const auto code_to = plugin_main_sub_settings::make_sub_code_key(i);
        const char* c = obs_data_get_string(s, code_from.data());
        obs_data_set_string(s, code_to.data(), c ? c : "");
        d->router.set_sub_code(i, c ? c : "");
    }
    {
        const auto delay_last = plugin_main_sub_settings::make_sub_delay_key(MAX_SUB_CH - 1);
        obs_data_set_double(s, delay_last.data(), 0.0);
        const auto adjust_last = plugin_main_sub_settings::make_sub_adjust_key(MAX_SUB_CH - 1);
        obs_data_set_double(s, adjust_last.data(), 0.0);
        const auto code_last = plugin_main_sub_settings::make_sub_code_key(MAX_SUB_CH - 1);
        obs_data_set_string(s, code_last.data(), "");
        d->router.set_sub_code(MAX_SUB_CH - 1, "");
    }
    obs_data_set_int(s, "sub_ch_count", next);
    obs_data_release(s);
    blog(LOG_INFO, "[obs-delay-stream] cb_sub_remove sub_ch_count %d -> %d (remove ch=%d)",
         cur, next, ch + 1);

    for (int i = ch; i < MAX_SUB_CH - 1; ++i) {
        d->sub[i].delay_ms = d->sub[i + 1].delay_ms;
        d->sub[i].adjust_ms = d->sub[i + 1].adjust_ms;
        plugin_main_audio_processing::apply_sub_delay_to_buffer(d, i);
        d->sub[i].measure.reset();
    }
    d->sub[MAX_SUB_CH - 1].delay_ms = 0.0f;
    d->sub[MAX_SUB_CH - 1].adjust_ms = 0.0f;
    plugin_main_audio_processing::apply_sub_delay_to_buffer(d, MAX_SUB_CH - 1);
    d->sub[MAX_SUB_CH - 1].measure.reset();

    d->sub_ch_count = next;
    d->router.set_active_channels(next);
    d->flow.set_active_channels(next);
    d->flow.reset();
    d->request_props_refresh("cb_sub_remove");
    return false;
}

void PropertiesBuilder::add_sub_channels_group() {
    if (!props_ || !d_) return;
    obs_properties_t* grp = obs_properties_create();
    int sub_count = d_->sub_ch_count;
    for (int i = 0; i < sub_count; ++i) {
        d_->btn_ctx[i] = { d_, i };

        const auto memo_key = plugin_main_sub_settings::make_sub_memo_key(i);
        char lt[32];
        snprintf(lt, sizeof(lt), "Ch.%d", i + 1);

        const auto row_prop = plugin_main_sub_settings::make_sub_remove_row_key(i);
        const bool input_enabled = !d_->router_running.load();
        const bool button_enabled = !(d_->router_running.load() || sub_count <= 1);
        obs_properties_add_text_button(
            grp, row_prop.data(), lt, memo_key.data(), T_("SubRemove"),
            cb_sub_remove, &d_->btn_ctx[i], input_enabled, button_enabled);
    }
    obs_property_t* spc_bottom = obs_properties_add_text(grp, "sub_add_spacer", "", OBS_TEXT_INFO);
    obs_property_set_long_description(spc_bottom, " ");
    obs_property_text_set_info_word_wrap(spc_bottom, false);
    char add_label[64];
    if (d_->sub_ch_count >= MAX_SUB_CH) {
        snprintf(add_label, sizeof(add_label), "%s", T_("SubAddLimitReached"));
    } else {
        snprintf(add_label, sizeof(add_label), T_("SubAddFmt"), d_->sub_ch_count + 1);
    }
    obs_property_t* add_p =
        obs_properties_add_button2(grp, "sub_add_btn", add_label, cb_sub_add, d_);
    if (d_->router_running.load() || d_->sub_ch_count >= MAX_SUB_CH) {
        obs_property_set_enabled(add_p, false);
    }
    obs_properties_add_group(props_, "grp_sub", T_("GroupSubChannels"), OBS_GROUP_NORMAL, grp);
}

} // namespace plugin_main_properties_ui
