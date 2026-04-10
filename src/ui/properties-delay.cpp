#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "ui/properties-delay.hpp"
#include "widgets/delay-table-widget.hpp"
#include "widgets/stepper-widget.hpp"

#include "core/string-format.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

#define T_(s) obs_module_text(s)

namespace ods::ui::delay {

	using ods::plugin::DelayStreamData;
	using ods::sync::FlowResult;
	using namespace ods::core;
	using namespace ods::widgets;

	void add_avatar_latency_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t *grp = obs_properties_create();
		{
			char info[256];
			snprintf(info, sizeof(info), "%s", T_("AvatarLatencyInfoFmt"));
			obs_property_t *info_p =
				obs_properties_add_text(grp, "avatar_latency_info", info, OBS_TEXT_INFO);
			obs_property_text_set_info_word_wrap(info_p, false);
		}
		obs_properties_add_stepper(
			grp,
			"avatar_latency_ms_stepper",
			T_("AvatarLatencyLabel"),
			ods::plugin::kAvatarLatencyKey,
			0.0,
			5000.0,
			0.0,
			0,
			" ms",
			true);
		obs_properties_add_group(props, "grp_avatar_latency", T_("GroupAvatarLatency"), OBS_GROUP_NORMAL, grp);
	}

	void add_delay_summary_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t *grp       = obs_properties_create();
		const int         sub_count = d->sub_ch_count;

		int         selected_ch = 0;
		obs_data_t *settings    = d->context ? obs_source_get_settings(d->context) : nullptr;
		if (settings) {
			selected_ch = static_cast<int>(obs_data_get_int(settings, "delay_table_selected_ch"));
			if (selected_ch < 0 || selected_ch >= sub_count)
				selected_ch = 0;
		}

		// 全チャンネルの raw 値を算出し neg_max を求める。
		const int R = d->measured_rtsp_e2e_ms;
		const int A = d->avatar_latency_ms;

		std::vector<int>  raw_vals(static_cast<size_t>(sub_count));
		std::vector<bool> has_meas(static_cast<size_t>(sub_count));
		int               neg_max = 0;
		for (int i = 0; i < sub_count; ++i) {
			has_meas[i] = d->sub_channels[i].ws_measured;
			if (has_meas[i]) {
				raw_vals[i] = ods::plugin::calc_ch_raw_delay_ms(R, A, d->sub_channels[i].measured_ms, d->sub_channels[i].offset_ms);
				if (raw_vals[i] < 0 && -raw_vals[i] > neg_max)
					neg_max = -raw_vals[i];
			}
		}

		std::vector<DelayTableChannelInfo> channels(static_cast<size_t>(sub_count));
		for (int i = 0; i < sub_count; ++i) {
			const auto  memo_key     = ods::plugin::make_sub_memo_key(i);
			const char *memo         = settings ? obs_data_get_string(settings, memo_key.data()) : "";
			channels[i].name         = (memo && *memo) ? memo : "";
			channels[i].measured_ms  = has_meas[i] ? static_cast<float>(d->sub_channels[i].measured_ms) : -1.0f;
			channels[i].offset_ms    = d->sub_channels[i].offset_ms;
			channels[i].raw_delay_ms = has_meas[i] ? raw_vals[i] : 0;
			channels[i].neg_max_ms   = neg_max;
			channels[i].total_ms     = has_meas[i] ? (raw_vals[i] + neg_max) : 0;
			channels[i].warn         = has_meas[i] && neg_max > 0 && raw_vals[i] == -neg_max;
		}

		if (settings) obs_data_release(settings);

		DelayTableLabels labels;
		labels.hdr_ch       = T_("DelayTableColCh");
		labels.hdr_name     = T_("DelayTableColName");
		labels.hdr_measured = T_("DelayTableColMeasured");
		labels.hdr_adjust   = T_("DelayTableColAdjust");
		labels.hdr_raw      = T_("DelayTableColRaw");
		labels.hdr_floor    = T_("DelayTableColFloor");
		labels.hdr_total    = T_("DelayTableColTotal");
		labels.lbl_editor   = T_("DelayTableAdjustLabel");
		obs_properties_add_delay_table(
			grp,
			"delay_table",
			selected_ch,
			sub_count,
			channels.data(),
			labels);

		{
			const std::string obs_delay_text =
				string_printf(T_("ObsOutputDelayFmt"), neg_max);
			obs_properties_add_text(grp, "obs_output_delay", obs_delay_text.c_str(), OBS_TEXT_INFO);
		}

		obs_properties_add_group(
			props,
			"grp_delay_summary",
			T_("GroupDelaySummary"),
			OBS_GROUP_NORMAL,
			grp);
	}

} // namespace ods::ui::delay
