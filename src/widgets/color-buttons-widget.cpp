#include "widgets/color-buttons-widget.hpp"

#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
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
#include <unordered_set>
#include <vector>

namespace ods::widgets {

	namespace {

		constexpr char kColorButtonRowMagicPipe[]    = "CBTNROW|";
		constexpr int  kColorButtonRowInjectRetryMax = 80;
		constexpr int  kColorButtonRowInjectRetryMs  = 1;

		// プレースホルダー payload を解析した 1 ボタン分の定義。
		struct ParsedButtonSpec {
			QString label;
			bool    enabled = true;
			QString bg_color;
			QString text_color;
		};

		// binding_id ごとに隠し action ボタン群を引けるように保持する。
		std::unordered_map<std::string, std::vector<obs_property_t *>> g_color_button_action_props;
		std::mutex                                                     g_color_button_action_props_mutex;
		std::atomic<uint64_t>                                          g_color_button_binding_seq{1};

		/// タブ選択行の binding_id か判定する。
		bool is_tab_selector_binding_id(const std::string &binding_id) {
			return binding_id.rfind("tab_selector_row#", 0) == 0;
		}

		/// 2色の中間色を返す。
		QColor mix_color(const QColor &a, const QColor &b, int ratio_b_percent) {
			const int rb = std::clamp(ratio_b_percent, 0, 100);
			const int ra = 100 - rb;
			return QColor(
				(a.red() * ra + b.red() * rb) / 100,
				(a.green() * ra + b.green() * rb) / 100,
				(a.blue() * ra + b.blue() * rb) / 100,
				255);
		}

		/// ボタン色指定を含むスタイルシート文字列を組み立てる。
		QString make_button_style_sheet(const QString &bg_color,
										const QString &text_color,
										bool           tab_mode,
										bool           active_tab) {
			QStringList styles;
			if (tab_mode) {
				styles.push_back(QStringLiteral(
					"QPushButton { "
					"padding-left: 8px; padding-right: 8px; "
					"padding-top: 3px; padding-bottom: 3px; "
					"border: 1px solid palette(mid); "
					"border-bottom-color: palette(mid); "
					"border-top-left-radius: 6px; border-top-right-radius: 6px; "
					"border-bottom-left-radius: 0px; border-bottom-right-radius: 0px; "
					"}"));
			} else {
				styles.push_back(QStringLiteral(
					"QPushButton { padding-left: 3px; padding-right: 3px; }"));
			}

			QStringList enabled_styles;
			if (!bg_color.isEmpty())
				enabled_styles.push_back(QStringLiteral("background-color: ") + bg_color);
			if (!text_color.isEmpty())
				enabled_styles.push_back(QStringLiteral("color: ") + text_color);
			if (tab_mode && active_tab) {
				enabled_styles.push_back(QStringLiteral("border-color: palette(mid)"));
				enabled_styles.push_back(QStringLiteral("border-bottom-color: transparent"));
				enabled_styles.push_back(QStringLiteral("margin-bottom: -6px"));
				enabled_styles.push_back(QStringLiteral("padding-bottom: 9px"));
			}
			if (!enabled_styles.isEmpty()) {
				styles.push_back(QStringLiteral("QPushButton:enabled { ") +
								 enabled_styles.join(QStringLiteral("; ")) +
								 QStringLiteral("; }"));
			}
			return styles.join(QStringLiteral(" "));
		}

		/// 隠し action 群を引くための一意 binding_id を生成する。
		std::string make_color_button_binding_id(const char *prop_name) {
			std::string    base = prop_name ? prop_name : "";
			const uint64_t seq =
				g_color_button_binding_seq.fetch_add(1, std::memory_order_relaxed);
			char suffix[32];
			std::snprintf(suffix, sizeof(suffix), "#%llu", static_cast<unsigned long long>(seq));
			return base + suffix;
		}

		// 色付きボタン群を 1 行で提供する inject 後ウィジェット。
		class ColorButtonRow : public QWidget {
		public:

			ColorButtonRow(obs_source_t *source, const char *binding_id, const std::vector<ParsedButtonSpec> &buttons, const QString &status_dot_color, const QString &status_text, QWidget *parent = nullptr)
				: QWidget(parent),
				  source_(source ? obs_source_get_ref(source) : nullptr),
				  binding_id_(binding_id ? binding_id : "") {
				const bool tab_mode = is_tab_selector_binding_id(binding_id_);
				auto      *lay      = new QHBoxLayout(this);
				if (tab_mode) {
					// 先頭タブが直下グループの左上角丸に重ならないように左余白を確保する。
					lay->setContentsMargins(8, 0, 0, 0);
				} else {
					lay->setContentsMargins(0, 0, 0, 0);
				}
				lay->setSpacing(4);

				for (size_t i = 0; i < buttons.size(); ++i) {
					const auto &spec   = buttons[i];
					auto       *button = new QPushButton(spec.label);
					const bool  active_tab =
						tab_mode && spec.bg_color.isEmpty() && spec.text_color.isEmpty();
					QString effective_bg   = spec.bg_color;
					QString effective_text = spec.text_color;
					if (tab_mode && !active_tab) {
						// 非アクティブタブは「背景色」と「ボタン色」の中間色を使う。
						const QColor window_color = button->palette().color(QPalette::Window);
						const QColor button_color = button->palette().color(QPalette::Button);
						const QColor mixed_bg     = mix_color(window_color, button_color, 50);
						effective_bg              = mixed_bg.name(QColor::HexRgb);
						// 文字色はテーマ文字色に非アクティブ背景色を 30% 混ぜて少しだけ薄くする。
						const QColor base_text_color = button->palette().color(QPalette::ButtonText);
						const QColor mixed_text      = mix_color(base_text_color, mixed_bg, 30);
						effective_text               = mixed_text.name(QColor::HexRgb);
					}
					button->setStyleSheet(
						make_button_style_sheet(
							effective_bg,
							effective_text,
							tab_mode,
							active_tab));
					button->setEnabled(spec.enabled);
					connect(button, &QPushButton::clicked, this, [this, i]() {
						onButtonClicked(i);
					});
					lay->addWidget(button);
					buttons_.push_back(button);
				}

				// ステータス表示（丸アイコン＋テキスト）をボタン群の右側に追加する。
				if (!status_text.isEmpty()) {
					QString html;
					if (!status_dot_color.isEmpty()) {
						html = QStringLiteral("<span style='color: ") +
							   status_dot_color.toHtmlEscaped() +
							   QStringLiteral("'>&#9679;</span> ") +
							   status_text.toHtmlEscaped();
					} else {
						html = status_text.toHtmlEscaped();
					}
					auto *status_label = new QLabel(html);
					status_label->setTextFormat(Qt::RichText);
					// ボタン群との間に余白を確保する。
					status_label->setContentsMargins(8, 0, 0, 0);
					lay->addWidget(status_label);
				}

				// 余白側へストレッチを入れて、固定幅ボタン群を常に左寄せにする。
				lay->addStretch(1);
				updateButtonWidths();
				QTimer::singleShot(0, this, [this]() { updateButtonWidths(); });
			}

			~ColorButtonRow() override {
				{
					// inject 後の古い binding が残らないように破棄時に必ず掃除する。
					std::lock_guard<std::mutex> lock(g_color_button_action_props_mutex);
					g_color_button_action_props.erase(binding_id_);
				}
				if (source_) {
					obs_source_release(source_);
					source_ = nullptr;
				}
			}

		private:

			/// フォント/スタイル変更時にボタン幅を再計算する。
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

			/// ラベル幅に合わせて各ボタンの固定幅を更新する。
			void updateButtonWidths() {
				for (auto *button : buttons_) {
					if (!button)
						continue;
					const int width =
						button->fontMetrics().horizontalAdvance(button->text()) + 20;
					button->setFixedWidth(width);
				}
			}

			/// 対応する隠し action ボタンを押下し、必要ならプロパティ再描画する。
			void onButtonClicked(size_t index) {
				obs_property_t *action_prop = nullptr;
				{
					std::lock_guard<std::mutex> lock(g_color_button_action_props_mutex);
					auto                        it = g_color_button_action_props.find(binding_id_);
					if (it != g_color_button_action_props.end() &&
						index < it->second.size()) {
						action_prop = it->second[index];
					}
				}
				if (!action_prop)
					return;

				const bool need_refresh = obs_property_button_clicked(action_prop, source_);
				if (need_refresh && source_) {
					// obs_source_update_properties を次のイベントループへ延期する。
					// コールバックが return true を返した場合、呼び出し元の ColorButtonRow
					// 自身が obs_source_update_properties 内で同期的に delete される可能性があり、
					// メンバ関数の途中で this が解放される未定義動作を防ぐため。
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
		struct ColorButtonInjectCtx {
			explicit ColorButtonInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {
			}
			~ColorButtonInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kColorButtonRowInjectRetryMax;
		};

		/// "1"/それ以外を bool に変換する。
		bool parse_bool_field(const QString &value) {
			return value == QLatin1String("1");
		}

		/// 色ボタン行プレースホルダー payload を分解する。
		bool parse_color_button_payload(const QString                 &text,
										QString                       &binding_id,
										QString                       &row_label,
										std::vector<ParsedButtonSpec> &buttons,
										QString                       &status_dot_color,
										QString                       &status_text,
										QString                       &label_color) {
			QStringList fields;
			if (!split_escaped_pipe_fields(text, fields))
				return false;
			if (fields.size() < 4 || fields[0] != QLatin1String("CBTNROW"))
				return false;

			bool      ok    = false;
			const int count = fields[3].toInt(&ok);
			if (!ok || count <= 0)
				return false;

			const int expected_fields = 4 + count * 4;
			if (fields.size() < expected_fields)
				return false;

			binding_id = fields[1];
			row_label  = fields[2];
			buttons.clear();
			buttons.reserve(static_cast<size_t>(count));
			int idx = 4;
			for (int i = 0; i < count; ++i) {
				ParsedButtonSpec spec;
				spec.label      = fields[idx++];
				spec.enabled    = parse_bool_field(fields[idx++]);
				spec.bg_color   = fields[idx++];
				spec.text_color = fields[idx++];
				buttons.push_back(spec);
			}
			// ステータス表示用の 2 フィールドはオプション（後方互換）。
			status_dot_color = (fields.size() >= expected_fields + 2) ? fields[expected_fields] : QString{};
			status_text      = (fields.size() >= expected_fields + 2) ? fields[expected_fields + 1] : QString{};
			// ラベル色フィールドはオプション。
			label_color = (fields.size() >= expected_fields + 3) ? fields[expected_fields + 2] : QString{};
			return true;
		}

		/// 色ボタン行プレースホルダーを実ウィジェットへ置換する。
		void do_color_button_row_inject(void *param) {
			auto ctx = std::unique_ptr<ColorButtonInjectCtx>(static_cast<ColorButtonInjectCtx *>(param));
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
				if (text.startsWith(QLatin1String(kColorButtonRowMagicPipe)))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found)
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

			int replaced_count = 0;
			for (const auto &ph : found) {
				QString                       binding_id;
				QString                       row_label;
				std::vector<ParsedButtonSpec> buttons;
				QString                       status_dot_color;
				QString                       status_text;
				QString                       label_color;
				if (!parse_color_button_payload(ph.text, binding_id, row_label, buttons, status_dot_color, status_text, label_color))
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

				auto *row_widget = new ColorButtonRow(
					ctx->source,
					binding_id.toUtf8().constData(),
					buttons,
					status_dot_color,
					status_text,
					parent);

				form->removeRow(row);
				if (!row_label.isEmpty()) {
					QColor color(label_color);
					if (color.isValid())
						form->insertRow(row, create_colored_label(row_label, color, parent), row_widget);
					else
						form->insertRow(row, row_label, row_widget);
				} else {
					form->insertRow(row, row_widget);
				}
				if (is_tab_selector_binding_id(binding_id.toStdString())) {
					// タブ行と表示中グループの行間を詰める。
					form->setVerticalSpacing(0);
				}
				++replaced_count;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				// UI ツリー反映待ちの取りこぼしを吸収するため短時間で再試行する。
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kColorButtonRowInjectRetryMs,
								   [next]() { do_color_button_row_inject(next); });
				return;
			}
		}

	} // namespace

	obs_property_t *obs_properties_add_color_button_row(
		obs_properties_t         *props,
		const char               *prop_name,
		const char               *label,
		const ObsColorButtonSpec *buttons,
		size_t                    button_count,
		const char               *status_dot_color,
		const char               *status_text,
		const char               *label_color) {
		if (!props || !prop_name || !*prop_name || !buttons || button_count == 0)
			return nullptr;
		if (obs_properties_get(props, prop_name))
			return nullptr;

		std::unordered_set<std::string> action_names;
		action_names.reserve(button_count);
		for (size_t i = 0; i < button_count; ++i) {
			const auto &spec = buttons[i];
			if (!spec.action_prop_name || !*spec.action_prop_name ||
				!spec.button_label || !*spec.button_label ||
				!spec.clicked) {
				return nullptr;
			}
			const std::string action_name(spec.action_prop_name);
			if (action_name == prop_name)
				return nullptr;
			if (obs_properties_get(props, action_name.c_str()))
				return nullptr;
			if (!action_names.insert(action_name).second)
				return nullptr;
		}

		std::vector<obs_property_t *> action_props;
		action_props.reserve(button_count);
		for (size_t i = 0; i < button_count; ++i) {
			const auto     &spec        = buttons[i];
			obs_property_t *action_prop = obs_properties_add_button2(
				props,
				spec.action_prop_name,
				spec.button_label,
				spec.clicked,
				spec.clicked_priv);
			if (!action_prop)
				return nullptr;
			obs_property_set_visible(action_prop, false);
			obs_property_set_enabled(action_prop, spec.enabled);
			action_props.push_back(action_prop);
		}

		const std::string binding_id = make_color_button_binding_id(prop_name);

		std::string payload = "CBTNROW|";
		payload += escape_field(binding_id.c_str());
		payload += "|";
		payload += escape_field(label ? label : "");
		payload += "|";
		payload += std::to_string(button_count);

		for (size_t i = 0; i < button_count; ++i) {
			const auto &spec = buttons[i];
			payload += "|";
			payload += escape_field(spec.button_label);
			payload += "|";
			payload += (spec.enabled ? "1" : "0");
			payload += "|";
			payload += escape_field(spec.bg_color ? spec.bg_color : "");
			payload += "|";
			payload += escape_field(spec.text_color ? spec.text_color : "");
		}

		// ステータス表示フィールドを末尾に追加する（オプション）。
		payload += "|";
		payload += escape_field(status_dot_color ? status_dot_color : "");
		payload += "|";
		payload += escape_field(status_text ? status_text : "");
		// ラベル色フィールド（オプション）。
		payload += "|";
		payload += escape_field(label_color ? label_color : "");

		obs_property_t *placeholder =
			obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
		if (!placeholder)
			return nullptr;

		{
			std::lock_guard<std::mutex> lock(g_color_button_action_props_mutex);
			g_color_button_action_props[binding_id] = action_props;
		}
		return placeholder;
	}

	void schedule_color_button_row_inject(obs_source_t *source) {
		if (!source)
			return;
		auto ctx = std::make_unique<ColorButtonInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_color_button_row_inject, ctx.release(), false);
	}

} // namespace ods::widgets
