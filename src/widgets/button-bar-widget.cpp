#include "widgets/button-bar-widget.hpp"

#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QApplication>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ods::widgets {

	namespace {

		constexpr char kButtonBarMagicPipe[]    = "BTNBAR|";
		constexpr int  kButtonBarInjectRetryMax = 40;
		constexpr int  kButtonBarInjectRetryMs  = 5;

		// binding_id ごとに隠し action プロパティ群を解決するためのレジストリ。
		std::unordered_map<std::string, std::vector<obs_property_t *>> g_button_bar_action_props;
		std::mutex                                                     g_button_bar_action_props_mutex;
		std::atomic<uint64_t>                                          g_button_bar_binding_seq{1};

		// payload から解析した 1 ボタン分の定義。
		struct ParsedButtonSpec {
			QString label;
			bool    enabled = true;
		};

		// action 参照レジストリ用の一意 binding_id を生成する。
		std::string make_button_bar_binding_id(const char *prop_name) {
			std::string    base = prop_name ? prop_name : "";
			const uint64_t seq =
				g_button_bar_binding_seq.fetch_add(1, std::memory_order_relaxed);
			char suffix[32];
			std::snprintf(suffix, sizeof(suffix), "#%llu", static_cast<unsigned long long>(seq));
			return base + suffix;
		}

		// 左寄せ・右寄せでボタンを配置する inject 後ウィジェット。
		class ButtonBarRow : public QWidget {
		public:

			ButtonBarRow(obs_source_t                        *source,
						 const char                          *binding_id,
						 const std::vector<ParsedButtonSpec> &left_specs,
						 const std::vector<ParsedButtonSpec> &right_specs,
						 QWidget                             *parent = nullptr)
				: QWidget(parent),
				  source_(source ? obs_source_get_ref(source) : nullptr),
				  binding_id_(binding_id ? binding_id : "") {

				auto *lay = new QHBoxLayout(this);
				lay->setContentsMargins(0, 0, 0, 0);
				lay->setSpacing(4);

				int action_idx = 0;
				for (const auto &spec : left_specs) {
					auto *btn = new QPushButton(spec.label);
					btn->setEnabled(spec.enabled);
					const int idx = action_idx++;
					connect(btn, &QPushButton::clicked, this, [this, idx]() { onButtonClicked(idx); });
					lay->addWidget(btn);
					buttons_.push_back(btn);
				}

				lay->addStretch(1);

				for (const auto &spec : right_specs) {
					auto *btn = new QPushButton(spec.label);
					btn->setEnabled(spec.enabled);
					const int idx = action_idx++;
					connect(btn, &QPushButton::clicked, this, [this, idx]() { onButtonClicked(idx); });
					lay->addWidget(btn);
					buttons_.push_back(btn);
				}

				updateButtonWidths();
				QTimer::singleShot(0, this, [this]() { updateButtonWidths(); });
			}

			~ButtonBarRow() override {
				if (source_) {
					obs_source_release(source_);
					source_ = nullptr;
				}
			}

		private:

			bool event(QEvent *e) override {
				switch (e->type()) {
				case QEvent::FontChange:
				case QEvent::StyleChange:
				case QEvent::Polish:
					updateButtonWidths();
					break;
				default:
					break;
				}
				return QWidget::event(e);
			}

			void updateButtonWidths() {
				for (auto *btn : buttons_) {
					const int text_w =
						btn->fontMetrics().horizontalAdvance(btn->text());
					// sizeHint の幅とテキスト幅+余白のうち大きい方を採用する。
					const int hint_w   = btn->sizeHint().width();
					const int manual_w = text_w + 24;
					btn->setFixedWidth(std::max(hint_w, manual_w));
				}
			}

			void onButtonClicked(int action_idx) {
				obs_property_t *action_prop = nullptr;
				{
					std::lock_guard<std::mutex> lock(g_button_bar_action_props_mutex);
					auto                        it = g_button_bar_action_props.find(binding_id_);
					if (it != g_button_bar_action_props.end() &&
						action_idx < static_cast<int>(it->second.size())) {
						action_prop = it->second[static_cast<size_t>(action_idx)];
					}
				}
				if (!action_prop)
					return;

				const bool need_refresh = obs_property_button_clicked(action_prop, source_);
				if (need_refresh && source_) {
					obs_source_t *src = obs_source_get_ref(source_);
					if (src) {
						QTimer::singleShot(0, qApp, [src]() {
							obs_source_update_properties(src);
							obs_source_release(src);
						});
					}
				}
			}

			obs_source_t              *source_ = nullptr;
			std::string                binding_id_;
			std::vector<QPushButton *> buttons_;
		};

		// inject 再試行状態を保持する UI タスク用コンテキスト。
		struct ButtonBarInjectCtx {
			explicit ButtonBarInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {
			}
			~ButtonBarInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kButtonBarInjectRetryMax;
		};

		// ボタンバー payload を分解する。
		// 形式: BTNBAR|binding_id|label|left_count|right_count|
		//        btn0_label|btn0_enabled|btn1_label|btn1_enabled|...
		// 左ボタン群が先、右ボタン群が後。
		bool parse_button_bar_payload(const QString                 &text,
									  QString                       &binding_id,
									  QString                       &row_label,
									  std::vector<ParsedButtonSpec> &left_specs,
									  std::vector<ParsedButtonSpec> &right_specs) {
			QStringList fields;
			if (!split_escaped_pipe_fields(text, fields))
				return false;
			if (fields.size() < 5 || fields[0] != QLatin1String("BTNBAR"))
				return false;

			binding_id      = fields[1];
			row_label       = fields[2];
			const int l_cnt = fields[3].toInt();
			const int r_cnt = fields[4].toInt();
			const int total = l_cnt + r_cnt;

			if (fields.size() < 5 + total * 2)
				return false;

			int idx = 5;
			left_specs.reserve(l_cnt);
			for (int i = 0; i < l_cnt; ++i) {
				ParsedButtonSpec spec;
				spec.label   = fields[idx++];
				spec.enabled = (fields[idx++] == QLatin1String("1"));
				left_specs.push_back(std::move(spec));
			}
			right_specs.reserve(r_cnt);
			for (int i = 0; i < r_cnt; ++i) {
				ParsedButtonSpec spec;
				spec.label   = fields[idx++];
				spec.enabled = (fields[idx++] == QLatin1String("1"));
				right_specs.push_back(std::move(spec));
			}
			return true;
		}

		// ボタンバープレースホルダーを実ウィジェットへ置換する。
		void do_button_bar_inject(void *param) {
			auto ctx = std::unique_ptr<ButtonBarInjectCtx>(static_cast<ButtonBarInjectCtx *>(param));
			if (!ctx)
				return;

			struct Placeholder {
				QLabel *label = nullptr;
				QString text;
			};
			std::vector<Placeholder>    found;
			std::vector<ScrollSnapshot> scroll_snapshots;

			const auto all_widgets = QApplication::allWidgets();
			for (QWidget *w : all_widgets) {
				auto *lbl = qobject_cast<QLabel *>(w);
				if (!lbl)
					continue;
				const QString text = lbl->text();
				if (text.startsWith(QLatin1String(kButtonBarMagicPipe)))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found)
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

			int replaced_count = 0;
			for (const auto &ph : found) {
				QString                       binding_id;
				QString                       row_label;
				std::vector<ParsedButtonSpec> left_specs;
				std::vector<ParsedButtonSpec> right_specs;
				if (!parse_button_bar_payload(ph.text, binding_id, row_label, left_specs, right_specs))
					continue;

				QWidget *parent = ph.label->parentWidget();
				if (!parent)
					continue;
				auto *form = qobject_cast<QFormLayout *>(parent->layout());
				if (!form)
					continue;

				int                   row = -1;
				QFormLayout::ItemRole role;
				form->getWidgetPosition(ph.label, &row, &role);
				if (row < 0)
					continue;

				auto *row_widget = new ButtonBarRow(
					ctx->source,
					binding_id.toUtf8().constData(),
					left_specs,
					right_specs,
					parent);

				form->removeRow(row);
				if (!row_label.isEmpty())
					form->insertRow(row, row_label, row_widget);
				else
					form->insertRow(row, row_widget);
				++replaced_count;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kButtonBarInjectRetryMs,
								   [next]() { do_button_bar_inject(next); });
				return;
			}
		}

	} // namespace

	obs_property_t *obs_properties_add_button_bar(
		obs_properties_t       *props,
		const char             *prop_name,
		const char             *label,
		const ObsButtonBarSpec *left_buttons,
		size_t                  left_count,
		const ObsButtonBarSpec *right_buttons,
		size_t                  right_count) {
		if (!props || !prop_name || !*prop_name)
			return nullptr;
		if (obs_properties_get(props, prop_name))
			return nullptr;

		// 各ボタンの action プロパティを登録する（左→右の順）。
		std::vector<obs_property_t *> action_props;
		auto                          register_buttons = [&](const ObsButtonBarSpec *buttons, size_t count) {
			for (size_t i = 0; i < count; ++i) {
				const auto &spec = buttons[i];
				if (!spec.action_prop_name || !spec.button_label || !spec.clicked)
					return false;
				if (obs_properties_get(props, spec.action_prop_name))
					return false;
				obs_property_t *action_prop = obs_properties_add_button2(
					props,
					spec.action_prop_name,
					spec.button_label,
					spec.clicked,
					spec.clicked_priv);
				if (!action_prop)
					return false;
				obs_property_set_visible(action_prop, false);
				obs_property_set_enabled(action_prop, spec.enabled);
				action_props.push_back(action_prop);
			}
			return true;
		};

		if (left_buttons && left_count > 0) {
			if (!register_buttons(left_buttons, left_count))
				return nullptr;
		}
		if (right_buttons && right_count > 0) {
			if (!register_buttons(right_buttons, right_count))
				return nullptr;
		}

		const std::string binding_id = make_button_bar_binding_id(prop_name);
		const size_t      l_cnt      = (left_buttons ? left_count : 0);
		const size_t      r_cnt      = (right_buttons ? right_count : 0);

		// payload 組み立て: BTNBAR|binding_id|label|left_count|right_count|btn...
		std::string payload = "BTNBAR|";
		payload += escape_field(binding_id.c_str());
		payload += "|";
		payload += escape_field(label ? label : "");
		payload += "|";
		payload += std::to_string(l_cnt);
		payload += "|";
		payload += std::to_string(r_cnt);

		auto append_buttons = [&](const ObsButtonBarSpec *buttons, size_t count) {
			for (size_t i = 0; i < count; ++i) {
				payload += "|";
				payload += escape_field(buttons[i].button_label);
				payload += "|";
				payload += (buttons[i].enabled ? "1" : "0");
			}
		};
		if (left_buttons && l_cnt > 0)
			append_buttons(left_buttons, l_cnt);
		if (right_buttons && r_cnt > 0)
			append_buttons(right_buttons, r_cnt);

		obs_property_t *placeholder =
			obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
		if (!placeholder)
			return nullptr;

		{
			std::lock_guard<std::mutex> lock(g_button_bar_action_props_mutex);
			const std::string           prefix = std::string(prop_name) + "#";
			for (auto it = g_button_bar_action_props.begin();
				 it != g_button_bar_action_props.end();) {
				if (it->first != binding_id &&
					it->first.compare(0, prefix.size(), prefix) == 0) {
					it = g_button_bar_action_props.erase(it);
				} else {
					++it;
				}
			}
			g_button_bar_action_props[binding_id] = action_props;
		}
		return placeholder;
	}

	void schedule_button_bar_inject(obs_source_t *source) {
		if (!source)
			return;
		auto ctx = std::make_unique<ButtonBarInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_button_bar_inject, ctx.release(), false);
	}

} // namespace ods::widgets
