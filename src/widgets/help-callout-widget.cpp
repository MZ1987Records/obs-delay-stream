#include "widgets/help-callout-widget.hpp"
#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <obs-module.h>

#include <QApplication>
#include <QFormLayout>
#include <QLabel>
#include <QTimer>
#include <memory>
#include <string>
#include <vector>

namespace ods::widgets {

	namespace {

		constexpr char kCalloutMagic[]        = "HCALLOUT|";
		constexpr int  kCalloutInjectRetryMax = 40;
		constexpr int  kCalloutInjectRetryMs  = 5;

		struct CalloutInjectCtx {
			explicit CalloutInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {}
			~CalloutInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kCalloutInjectRetryMax;
		};

		// プレースホルダーをコールアウト QLabel へ差し替える。
		void do_callout_inject(void *param) {
			auto ctx = std::unique_ptr<CalloutInjectCtx>(
				static_cast<CalloutInjectCtx *>(param));
			if (!ctx) return;

			struct Placeholder {
				QLabel *label = nullptr;
				QString text;
			};
			std::vector<Placeholder>    found;
			std::vector<ScrollSnapshot> scroll_snapshots;

			const auto all_widgets = QApplication::allWidgets();
			for (QWidget *w : all_widgets) {
				auto *lbl = qobject_cast<QLabel *>(w);
				if (!lbl) continue;
				const QString text = lbl->text();
				if (text.startsWith(QLatin1String(kCalloutMagic)))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found)
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

			int replaced_count = 0;
			for (const auto &ph : found) {
				// ペイロード: HCALLOUT|<escaped_text>
				QStringList fields;
				if (!split_escaped_pipe_fields(ph.text, fields))
					continue;
				if (fields.size() < 2 || fields[0] != QLatin1String("HCALLOUT"))
					continue;
				const QString callout_text = fields[1];
				if (callout_text.isEmpty())
					continue;

				QWidget *parent = ph.label->parentWidget();
				if (!parent) continue;
				auto *form = qobject_cast<QFormLayout *>(parent->layout());
				if (!form) continue;

				int                   row = -1;
				QFormLayout::ItemRole role;
				form->getWidgetPosition(ph.label, &row, &role);
				if (row < 0) continue;

				auto *callout = create_help_callout(callout_text, parent);
				form->removeRow(row);
				form->insertRow(row, callout);
				++replaced_count;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kCalloutInjectRetryMs,
								   [next]() { do_callout_inject(next); });
			}
		}

	} // namespace

	obs_property_t *obs_properties_add_help_callout(
		obs_properties_t *props,
		const char       *prop_name,
		const char       *text) {
		if (!props || !prop_name || !*prop_name)
			return nullptr;

		std::string payload = "HCALLOUT";
		payload += '|';
		payload += escape_field(text ? text : "");

		return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
	}

	void schedule_help_callout_inject(obs_source_t *source) {
		if (!source) return;
		auto ctx = std::make_unique<CalloutInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_callout_inject, ctx.release(), false);
	}

} // namespace ods::widgets
