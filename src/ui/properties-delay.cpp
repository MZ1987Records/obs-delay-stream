#include "ui/properties-builder.hpp"

#include <cstdio>
#include <vector>

#include "widgets/delay-table-widget.hpp"
#include "plugin/plugin-state.hpp"
#include "plugin/plugin-settings.hpp"
#include "widgets/stepper-widget.hpp"

#define T_(s) obs_module_text(s)

namespace plugin_main_properties_ui {

void PropertiesBuilder::add_sub_offset_group() {
    if (!props_ || !d_) return;
    obs_properties_t* grp = obs_properties_create();
    {
        char offset_info[256];
        snprintf(offset_info, sizeof(offset_info), "%s", T_("GlobalOffsetInfoFmt"));
        obs_property_t* offset_info_p =
            obs_properties_add_text(grp, "sub_offset_info", offset_info, OBS_TEXT_INFO);
        obs_property_text_set_info_word_wrap(offset_info_p, false);
    }
    obs_properties_add_stepper(
        grp, "sub_offset_ms_stepper",
        T_("GlobalOffsetLabel"), "sub_offset_ms",
        -2000.0, 5000.0, 0.0, 0, " ms");
    obs_properties_add_group(props_, "grp_offset",
        T_("GroupGlobalOffset"), OBS_GROUP_NORMAL, grp);
}

void PropertiesBuilder::add_delay_summary_group() {
    if (!props_ || !d_) return;
    obs_properties_t* grp = obs_properties_create();
    const int sub_count = d_->sub_ch_count;

    int selected_ch = 0;
    obs_data_t* settings = d_->context ? obs_source_get_settings(d_->context) : nullptr;
    if (settings) {
        selected_ch = static_cast<int>(obs_data_get_int(settings, "delay_table_selected_ch"));
        if (selected_ch < 0 || selected_ch >= sub_count)
            selected_ch = 0;
    }

    const FlowResult flow_res = d_->flow.result();
    std::vector<DelayTableChannelInfo> channels(static_cast<size_t>(sub_count));
    for (int i = 0; i < sub_count; ++i) {
        const auto memo_key = plugin_settings::make_sub_memo_key(i);
        const char* memo = settings ? obs_data_get_string(settings, memo_key.data()) : "";
        channels[i].name        = (memo && *memo) ? memo : "";
        channels[i].measured_ms = flow_res.channels[i].measured
                                    ? (float)flow_res.channels[i].one_way_latency_ms
                                    : -1.0f;
        channels[i].base_ms     = d_->sub[i].delay_ms;
        channels[i].adjust_ms   = d_->sub[i].adjust_ms;
        channels[i].global_ms   = d_->sub_offset_ms;
        const float raw = plugin_settings::calc_sub_delay_raw_value_ms(
            d_->sub[i].delay_ms, d_->sub[i].adjust_ms, d_->sub_offset_ms);
        channels[i].warn     = raw < 0.0f;
        const float latency = flow_res.channels[i].measured
                                ? (float)flow_res.channels[i].one_way_latency_ms
                                : 0.0f;
        channels[i].total_ms = latency + plugin_settings::calc_effective_sub_delay_value_ms(
            d_->sub[i].delay_ms, d_->sub[i].adjust_ms, d_->sub_offset_ms);
    }

    if (settings) obs_data_release(settings);

    DelayTableLabels labels;
    labels.hdr_ch       = T_("DelayTableColCh");
    labels.hdr_name     = T_("DelayTableColName");
    labels.hdr_measured = T_("DelayTableColMeasured");
    labels.hdr_base     = T_("DelayTableColBase");
    labels.hdr_adjust   = T_("DelayTableColAdjust");
    labels.hdr_global   = T_("DelayTableColGlobal");
    labels.hdr_total    = T_("DelayTableColTotal");
    labels.lbl_editor   = T_("DelayTableAdjustLabel");
    obs_properties_add_delay_table(
        grp, "delay_table", selected_ch, sub_count, channels.data(), labels);

    obs_properties_add_group(
        props_, "grp_delay_summary", T_("GroupDelaySummary"), OBS_GROUP_NORMAL, grp);
}

} // namespace plugin_main_properties_ui
