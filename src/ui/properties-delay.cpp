#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "ui/properties-delay.hpp"
#include "viewmodel/delay-viewmodel.hpp"
#include "widgets/delay-diagram-widget.hpp"
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

	void add_delay_diagram_group(obs_properties_t *props, DelayStreamData *d, const DelayViewModel &vm) {
		if (!props || !d) return;
		obs_properties_t *grp       = obs_properties_create();
		const int         sub_count = static_cast<int>(vm.channels.size());

		// 想定アバター遅延ステッパー
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
			true,
			7,
			"#f59e0b");

		// タイミング図
		DelayDiagramInfo info{};
		info.R            = vm.rtsp_e2e_ms;
		info.A            = vm.avatar_latency_ms;
		info.buf          = vm.playback_buffer_ms;
		info.ch_count     = sub_count;
		info.master_delay = vm.snapshot.master_delay_ms;
		for (int i = 0; i < sub_count && i < DelayDiagramInfo::kMaxCh; ++i) {
			info.channels[i].measured_ms = vm.channels[i].measured_ms;
			info.channels[i].total_ms    = vm.channels[i].total_ms;
			info.channels[i].offset_ms   = vm.channels[i].offset_ms;
		}

		DelayDiagramLabels labels;
		labels.legend_delay      = T_("DiagramDelay");
		labels.legend_delay_desc = T_("DiagramDelayAutoCalcDesc");
		labels.legend_ws         = T_("DiagramWsLatency");
		labels.legend_env        = T_("DiagramEnvLatency");
		labels.legend_buf        = T_("DiagramPlaybackBuf");
		labels.legend_avatar     = T_("DiagramAvatarLatency");
		labels.legend_broadcast  = T_("DiagramBroadcastLatency");
		labels.lane_broadcast    = T_("DiagramLaneBroadcast");
		labels.no_data           = T_("DiagramNoData");
		labels.no_data_rtsp      = T_("DiagramNoDataRtsp");
		labels.no_data_ws        = T_("DiagramNoDataWs");
		obs_properties_add_delay_diagram(grp, "delay_diagram", info, labels);

		obs_properties_add_group(
			props,
			"grp_delay_diagram",
			T_("GroupDelayDiagram"),
			OBS_GROUP_NORMAL,
			grp);
	}

	void add_fine_tune_group(obs_properties_t *props, DelayStreamData *d, const DelayViewModel &vm) {
		if (!props || !d) return;
		obs_properties_t *grp       = obs_properties_create();
		const int         sub_count = static_cast<int>(vm.channels.size());

		// 想定環境遅延 テーブル
		std::vector<DelayTableChannelInfo> channels(static_cast<size_t>(sub_count));
		for (int i = 0; i < sub_count; ++i) {
			const auto &src         = vm.channels[i];
			channels[i].name        = src.name.c_str();
			channels[i].measured_ms = src.measured_ms;
			channels[i].offset_ms   = src.offset_ms;
			channels[i].total_ms    = src.total_ms;
		}

		// 合計 = R - A - B + neg_max（全チャンネル共通）
		const int sum_ms = vm.rtsp_e2e_ms - vm.avatar_latency_ms - vm.playback_buffer_ms + vm.snapshot.neg_max_ms;

		DelayTableLabels labels;
		labels.hdr_ch       = T_("DelayTableColCh");
		labels.hdr_name     = T_("DelayTableColName");
		labels.hdr_auto     = T_("DelayTableColAuto");
		labels.hdr_ws       = T_("DelayTableColWs");
		labels.hdr_env      = T_("DelayTableColEnv");
		labels.hdr_sum      = T_("DelayTableColSum");
		labels.lbl_editor   = T_("DelayTableAdjustLabel");
		labels.editor_color = "#8b5cf6";
		labels.help_text    = T_("EnvLatencyHelpText");
		obs_properties_add_delay_table(
			grp,
			"delay_table",
			vm.selected_ch,
			sub_count,
			channels.data(),
			sum_ms,
			labels);

		obs_properties_add_group(
			props,
			"grp_fine_tune",
			T_("GroupFineTune"),
			OBS_GROUP_NORMAL,
			grp);
	}

} // namespace ods::ui::delay
