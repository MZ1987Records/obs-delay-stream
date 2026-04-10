#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "ui/properties-delay.hpp"
#include "viewmodel/delay-viewmodel.hpp"
#include "widgets/delay-table-widget.hpp"
#include "widgets/stepper-widget.hpp"

#include "core/string-format.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

#define T_(s) obs_module_text(s)

namespace ods::ui::delay {

	using ods::plugin::DelayStreamData;
	using ods::viewmodel::DelayViewModel;
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

	void add_delay_summary_group(obs_properties_t *props, const DelayViewModel &vm) {
		if (!props) return;
		obs_properties_t *grp       = obs_properties_create();
		const int         sub_count = static_cast<int>(vm.channels.size());

		std::vector<DelayTableChannelInfo> channels(static_cast<size_t>(sub_count));
		for (int i = 0; i < sub_count; ++i) {
			const auto &src      = vm.channels[i];
			channels[i].name         = src.name.c_str();
			channels[i].measured_ms  = src.measured_ms;
			channels[i].offset_ms    = src.offset_ms;
			channels[i].raw_delay_ms = src.raw_delay_ms;
			channels[i].neg_max_ms   = src.neg_max_ms;
			channels[i].total_ms     = src.total_ms;
			channels[i].warn         = src.warn;
		}

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
			vm.selected_ch,
			sub_count,
			channels.data(),
			labels);

		{
			const std::string obs_delay_text =
				string_printf(T_("ObsOutputDelayFmt"), vm.snapshot.neg_max_ms);
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
