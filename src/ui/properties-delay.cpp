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
		const auto        snap      = d->delay.calc_all_delays();
		const int         sub_count = snap.active_count;

		int         selected_ch = 0;
		obs_data_t *settings    = d->context ? obs_source_get_settings(d->context) : nullptr;
		if (settings) {
			selected_ch = static_cast<int>(obs_data_get_int(settings, "delay_table_selected_ch"));
			if (selected_ch < 0 || selected_ch >= sub_count)
				selected_ch = 0;
		}

		std::vector<DelayTableChannelInfo> channels(static_cast<size_t>(sub_count));
		for (int i = 0; i < sub_count; ++i) {
			const auto &sch          = snap.channels[i];
			const auto  memo_key     = ods::plugin::make_sub_memo_key(i);
			const char *memo         = settings ? obs_data_get_string(settings, memo_key.data()) : "";
			channels[i].name         = (memo && *memo) ? memo : "";
			channels[i].measured_ms  = sch.has_measurement ? static_cast<float>(d->delay.channels[i].measured_ms) : -1.0f;
			channels[i].offset_ms    = d->delay.channels[i].offset_ms;
			channels[i].raw_delay_ms = sch.has_measurement ? sch.raw_ms : 0;
			channels[i].neg_max_ms   = snap.neg_max_ms;
			channels[i].total_ms     = sch.has_measurement ? sch.total_ms : 0;
			channels[i].warn         = sch.warn;
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
				string_printf(T_("ObsOutputDelayFmt"), snap.neg_max_ms);
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
