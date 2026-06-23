#include "model/settings-repo.hpp"
#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "ui/props-refresh.hpp"
#include "ui/properties-delay.hpp"
#include "viewmodel/delay-viewmodel.hpp"
#include "widgets/color-buttons-widget.hpp"
#include "widgets/delay-diagram-widget.hpp"
#include "widgets/delay-table-widget.hpp"
#include "widgets/help-callout-widget.hpp"
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

	namespace {

		// 生演奏 ON/OFF に応じてグループ表示を切り替える（チェックボックスはグループ外に配置）。
		bool cb_live_perf_enabled_changed(
			void             *priv,
			obs_properties_t *props,
			obs_property_t *,
			obs_data_t *settings) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!props || !settings || !d) return false;
			const ods::model::SettingsRepo repo(settings);
			if (auto *p = obs_properties_get(props, "grp_live_perf"))
				obs_property_set_visible(p, repo.live_perf_enabled());

			props_ui_with_preserved_scroll([d]() {
				if (!d || !d->context) return;
				schedule_color_button_row_inject(d->context);
				schedule_stepper_inject(d->context);
				schedule_help_callout_inject(d->context);
				schedule_delay_diagram_inject(d->context);
				schedule_delay_table_inject(d->context);
			});
			return true;
		}

	} // namespace

	void add_live_perf_group(
		obs_properties_t     *props,
		DelayStreamData      *d,
		const DelayViewModel &vm) {
		if (!props || !d) return;

		// チェックボックス（グループの外に配置）
		obs_property_t *enabled_p = obs_properties_add_bool(
			props,
			ods::plugin::kLivePerfEnabledKey,
			T_("LivePerfEnableLabel"));
		obs_property_set_modified_callback2(enabled_p, cb_live_perf_enabled_changed, d);

		obs_properties_t *grp = obs_properties_create();
		obs_properties_add_stepper(
			grp,
			"live_perf_lead_stepper",
			T_("LivePerfLeadLabel"),
			ods::plugin::kLivePerfLeadKey,
			0.0,
			10000.0,
			500.0,
			0,
			" ms",
			true,
			7,
			"#000000");
		obs_properties_add_help_callout(
			grp,
			"live_perf_lead_help",
			T_("LivePerfLeadHelpText"));
		obs_properties_add_help_callout(grp, "live_perf_desc", T_("LivePerfDesc"));

		if (vm.live_service_too_slow) {
			obs_properties_add_help_callout(
				grp,
				"live_perf_error_service_slow",
				T_("LivePerfErrServiceSlow"),
				CalloutVariant::Error);
		} else if (vm.live_lead_too_short) {
			const std::string warning =
				string_printf(T_("LivePerfErrLeadShort"), vm.live_min_lead_ms);
			obs_properties_add_help_callout(
				grp,
				"live_perf_error_lead_short",
				warning.c_str(),
				CalloutVariant::Warning);
		}

		obs_property_t *grp_p = obs_properties_add_group(
			props,
			"grp_live_perf",
			T_("GroupLivePerf"),
			OBS_GROUP_NORMAL,
			grp);
		// 初期状態: チェックされていなければグループを非表示
		obs_property_set_visible(grp_p, vm.live_perf_enabled);
	}

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
			"#f59e0b",
			T_("AvatarLatencyHelpText"));

		// タイミング図
		DelayDiagramInfo info{};
		info.R            = vm.rtsp_e2e_ms;
		info.A            = vm.avatar_latency_ms;
		info.buf          = vm.playback_buffer_ms;
		info.ch_count     = sub_count;
		info.master_delay = vm.snapshot.master_delay_ms;
		info.live_perf    = vm.live_perf_enabled;
		info.live_ok      = vm.live_perf_ok;
		info.lead_ms      = vm.lead_time_ms;
		for (int i = 0; i < sub_count && i < DelayDiagramInfo::kMaxCh; ++i) {
			info.channels[i].measured_ms = vm.channels[i].measured_ms;
			info.channels[i].total_ms    = vm.channels[i].total_ms;
			info.channels[i].offset_ms   = vm.channels[i].offset_ms;
			info.channels[i].provisional = vm.channels[i].provisional;
		}

		DelayDiagramLabels labels;
		labels.legend_delay         = T_("DiagramDelay");
		labels.legend_delay_desc    = T_("DiagramDelayAutoCalcDesc");
		labels.legend_ws            = T_("DiagramWsLatency");
		labels.legend_env           = T_("DiagramEnvLatency");
		labels.legend_buf           = T_("DiagramPlaybackBuf");
		labels.legend_avatar        = T_("DiagramAvatarLatency");
		labels.legend_broadcast     = T_("DiagramBroadcastLatency");
		labels.legend_lead          = T_("LivePerfLeadLabel");
		labels.lane_broadcast       = T_("DiagramLaneBroadcast");
		labels.lane_local           = T_("DiagramLaneLocal");
		labels.no_data              = T_("DiagramNoData");
		labels.no_data_rtsp         = T_("DiagramNoDataRtsp");
		labels.no_data_ws           = T_("DiagramNoDataWs");
		labels.legend_listen_timing = T_("DiagramListenTiming");
		labels.help_text            = T_("DiagramHelpText");
		obs_properties_add_delay_diagram(grp, "delay_diagram", info, labels);

		// 生演奏成立時: 本線は自動調整できないため、配信チャンネルの同期オフセットへ
		// 設定すべき値を全体調整グループ末尾に警告コールアウトで案内する。
		if (vm.live_perf_ok) {
			const int         baseline_ms        = REQUIRED_AUDIO_SYNC_OFFSET_MS;                 // -950
			const int         broadcast_delay_ms = vm.snapshot.master_delay_ms - vm.lead_time_ms; // A-R
			const int         sync_offset_ms     = baseline_ms + broadcast_delay_ms;              // 最終値
			const std::string guide              = string_printf(
				T_("LivePerfSyncOffsetGuide"),
				sync_offset_ms,
				baseline_ms,
				broadcast_delay_ms);
			obs_properties_add_help_callout(
				grp,
				"live_perf_sync_offset_guide",
				guide.c_str(),
				CalloutVariant::Warning);
		}

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
		const int sum_ms =
			vm.rtsp_e2e_ms - vm.avatar_latency_ms - vm.playback_buffer_ms +
			vm.snapshot.neg_max_ms + vm.live_extra_ms;

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
