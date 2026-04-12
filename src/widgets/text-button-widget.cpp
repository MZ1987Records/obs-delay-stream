#include "widgets/text-button-widget.hpp"

#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QApplication>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ods::widgets {

	namespace {

		constexpr char kTextButtonMagicPipe[]    = "TEXTBTN|";
		constexpr int  kTextButtonInjectRetryMax = 40;
		constexpr int  kTextButtonInjectRetryMs  = 5;

		// binding_id ごとに隠し action プロパティ群を解決するためのレジストリ。
		std::unordered_map<std::string, std::vector<obs_property_t *>> g_text_button_action_props;
		std::mutex                                                     g_text_button_action_props_mutex;
		std::atomic<uint64_t>                                          g_text_button_binding_seq{1};

		// 表示用プロパティ名とボタン番号から隠し action プロパティ名を作る。
		std::string make_text_button_action_name(const char *prop_name, size_t button_index) {
			std::string action_name = prop_name ? prop_name : "";
			action_name += "__textbtn_action_";
			action_name += std::to_string(button_index);
			return action_name;
		}

		// action 参照レジストリ用の一意 binding_id を生成する。
		std::string make_text_button_binding_id(const std::string &action_name) {
			const uint64_t seq =
				g_text_button_binding_seq.fetch_add(1, std::memory_order_relaxed);
			char suffix[32];
			std::snprintf(suffix, sizeof(suffix), "#%llu", static_cast<unsigned long long>(seq));
			return action_name + suffix;
		}

		// テキスト入力 + 実行ボタンを 1 行で提供する inject 後ウィジェット。
		class TextButtonRow : public QWidget {
		public:

			struct ButtonSpec {
				QString label;
				bool    enabled = true;
			};

			TextButtonRow(obs_source_t                  *source,
						  const char                    *key,
						  const char                    *binding_id,
						  const std::vector<ButtonSpec> &button_specs,
						  bool                           input_enabled,
						  int                            max_input_chars,
						  QWidget                       *parent = nullptr)
				: QWidget(parent),
				  source_(source ? obs_source_get_ref(source) : nullptr),
				  key_(key ? key : ""),
				  binding_id_(binding_id ? binding_id : "") {
				auto *lay = new QHBoxLayout(this);
				lay->setContentsMargins(0, 0, 0, 0);
				lay->setSpacing(4);

				edit_ = new QLineEdit();
				edit_->setMinimumWidth(0);
				edit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
				if (max_input_chars > 0)
					edit_->setMaxLength(max_input_chars);
				lay->addWidget(edit_, 1);

				connect(edit_, &QLineEdit::editingFinished, this, &TextButtonRow::onEditingFinished);
				for (size_t i = 0; i < button_specs.size(); ++i) {
					const auto &spec = button_specs[i];
					auto       *btn  = new QPushButton(spec.label);
					btn->setStyleSheet(QStringLiteral("padding-left: 3px; padding-right: 3px;"));
					btn->setEnabled(spec.enabled);
					lay->addWidget(btn);
					connect(btn, &QPushButton::clicked, this, [this, i]() { onButtonClicked(i); });
					buttons_.push_back(btn);
				}

				edit_->setEnabled(input_enabled);
				loadFromSettings();
				QTimer::singleShot(0, this, [this]() { updateButtonWidths(); });
			}

			~TextButtonRow() override {
				// binding_id のマップ削除はここでは行わない。
				// RefreshProperties 後の再 inject で同じ binding_id が必要になるため、
				// 古いエントリは次回の obs_properties_add_text_button_row で掃除される。
				if (source_) {
					obs_source_release(source_);
					source_ = nullptr;
				}
			}

		private:

			// フォント/スタイル変更時に実行ボタン幅を再計算する。
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

			// 実行ボタン群のテキスト幅に合わせて固定幅を更新する。
			void updateButtonWidths() {
				for (auto *button : buttons_) {
					if (!button)
						continue;
					const int width = button->fontMetrics().horizontalAdvance(button->text()) + 20;
					button->setFixedWidth(width);
				}
			}

			// 設定文字列を読み込み入力欄へ反映する。
			void loadFromSettings() {
				if (!source_ || !edit_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s)
					return;
				const char *text = obs_data_get_string(s, key_.c_str());
				obs_data_release(s);
				edit_->blockSignals(true);
				edit_->setText(QString::fromUtf8(text ? text : ""));
				edit_->blockSignals(false);
			}

			// 入力欄の値を設定へ同期する。
			void syncToSettings() {
				if (!source_ || !edit_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s)
					return;
				const QByteArray utf8 = edit_->text().toUtf8();
				obs_data_set_string(s, key_.c_str(), utf8.constData());
				obs_source_update(source_, s);
				obs_data_release(s);
			}

			// 編集確定時に設定へ同期する。
			void onEditingFinished() {
				syncToSettings();
			}

			// 設定同期後に隠し action ボタンを実行する。
			void onButtonClicked(size_t button_index) {
				syncToSettings();

				obs_property_t *action_prop = nullptr;
				{
					std::lock_guard<std::mutex> lock(g_text_button_action_props_mutex);
					auto                        it = g_text_button_action_props.find(binding_id_);
					if (it != g_text_button_action_props.end() &&
						button_index < it->second.size()) {
						action_prop = it->second[button_index];
					}
				}
				if (!action_prop)
					return;

				const bool need_refresh = obs_property_button_clicked(action_prop, source_);
				if (need_refresh && source_) {
					obs_source_update_properties(source_);
				}
			}

			obs_source_t              *source_ = nullptr;
			std::string                key_;
			std::string                binding_id_;
			QLineEdit                 *edit_ = nullptr;
			std::vector<QPushButton *> buttons_;
		};

		// inject 再試行状態を保持する UI タスク用コンテキスト。
		struct TextButtonInjectCtx {
			explicit TextButtonInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {
			}
			~TextButtonInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kTextButtonInjectRetryMax;
		};

		struct ParsedTextButtonPayload {
			QString                                key;
			QString                                binding_id;
			bool                                   input_enabled   = true;
			int                                    max_input_chars = 0;
			QString                                row_label;
			std::vector<TextButtonRow::ButtonSpec> buttons;
		};

		// テキスト+ボタン行プレースホルダー payload を分解する。
		bool parse_text_button_payload(const QString &text, ParsedTextButtonPayload &parsed) {
			QStringList fields;
			if (!split_escaped_pipe_fields(text, fields))
				return false;
			if (fields.empty() || fields[0] != QLatin1String("TEXTBTN"))
				return false;
			if (fields.size() >= 7) {
				bool      ok_button_count = false;
				const int button_count    = fields[6].toInt(&ok_button_count);
				if (ok_button_count && button_count > 0) {
					const int expected_fields = 7 + button_count * 2;
					if (fields.size() >= expected_fields) {
						parsed.key             = fields[1];
						parsed.binding_id      = fields[2];
						parsed.input_enabled   = (fields[3] == QLatin1String("1"));
						parsed.max_input_chars = fields[4].toInt();
						parsed.row_label       = fields[5];
						parsed.buttons.clear();
						parsed.buttons.reserve(static_cast<size_t>(button_count));
						int idx = 7;
						for (int i = 0; i < button_count; ++i) {
							TextButtonRow::ButtonSpec spec;
							spec.label   = fields[idx++];
							spec.enabled = (fields[idx++] == QLatin1String("1"));
							parsed.buttons.push_back(spec);
						}
						return true;
					}
				}
			}

			// 旧フォーマット互換: TEXTBTN|key|binding_id|button_label|input|button|max|label
			if (fields.size() < 5)
				return false;
			parsed.key        = fields[1];
			parsed.binding_id = fields[2];
			parsed.buttons.clear();
			TextButtonRow::ButtonSpec spec;
			spec.label = fields[3];
			if (fields.size() >= 8) {
				parsed.input_enabled   = (fields[4] == QLatin1String("1"));
				spec.enabled           = (fields[5] == QLatin1String("1"));
				parsed.max_input_chars = fields[6].toInt();
				parsed.row_label       = fields.mid(7).join(QStringLiteral(" "));
			} else if (fields.size() >= 7) {
				parsed.input_enabled = (fields[4] == QLatin1String("1"));
				spec.enabled         = (fields[5] == QLatin1String("1"));
				parsed.row_label     = fields.mid(6).join(QStringLiteral(" "));
			} else if (fields.size() >= 6) {
				const bool enabled   = (fields[4] == QLatin1String("1"));
				parsed.input_enabled = enabled;
				spec.enabled         = enabled;
				parsed.row_label     = fields.mid(5).join(QStringLiteral(" "));
			} else {
				parsed.row_label = fields.mid(4).join(QStringLiteral(" "));
			}
			parsed.buttons.push_back(spec);
			return true;
		}

		// テキスト+ボタン行プレースホルダーを実ウィジェットへ置換する。
		void do_text_button_inject(void *param) {
			auto ctx = std::unique_ptr<TextButtonInjectCtx>(static_cast<TextButtonInjectCtx *>(param));
			if (!ctx)
				return;

			struct Placeholder {
				QLabel *label;
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
				if (text.startsWith(QLatin1String(kTextButtonMagicPipe)))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found)
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

			int replaced_count = 0;
			for (auto &ph : found) {
				ParsedTextButtonPayload payload;
				if (!parse_text_button_payload(ph.text, payload))
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

				auto *row_widget = new TextButtonRow(
					ctx->source,
					payload.key.toUtf8().constData(),
					payload.binding_id.toUtf8().constData(),
					payload.buttons,
					payload.input_enabled,
					payload.max_input_chars,
					parent);

				form->removeRow(row);
				if (!payload.row_label.isEmpty())
					form->insertRow(row, payload.row_label, row_widget);
				else
					form->insertRow(row, row_widget);
				++replaced_count;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				// OBS プロパティ再構築直後は QLabel がまだ揃わない場合があるため再試行する。
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kTextButtonInjectRetryMs,
								   [next]() { do_text_button_inject(next); });
				return;
			}
		}

	} // namespace

	obs_property_t *obs_properties_add_text_buttons(obs_properties_t              *props,
													const char                    *prop_name,
													const char                    *label,
													const char                    *setting_key,
													const ObsTextButtonActionSpec *buttons,
													size_t                         button_count,
													bool                           input_enabled,
													int                            max_input_chars) {
		if (!props || !prop_name || !*prop_name || !setting_key || !*setting_key ||
			!buttons || button_count == 0) {
			return nullptr;
		}

		if (obs_properties_get(props, prop_name)) {
			return nullptr;
		}

		for (size_t i = 0; i < button_count; ++i) {
			const auto &spec = buttons[i];
			if (!spec.button_label || !*spec.button_label || !spec.clicked)
				return nullptr;
			const std::string action_name = make_text_button_action_name(prop_name, i);
			if (obs_properties_get(props, action_name.c_str()))
				return nullptr;
		}

		std::vector<obs_property_t *> action_props;
		action_props.reserve(button_count);
		for (size_t i = 0; i < button_count; ++i) {
			const auto       &spec        = buttons[i];
			const std::string action_name = make_text_button_action_name(prop_name, i);

			obs_property_t *action_prop = obs_properties_add_button2(
				props,
				action_name.c_str(),
				spec.button_label,
				spec.clicked,
				spec.clicked_priv);
			if (!action_prop)
				return nullptr;
			obs_property_set_visible(action_prop, false);
			action_props.push_back(action_prop);
		}

		const std::string binding_id = make_text_button_binding_id(prop_name);
		{
			std::lock_guard<std::mutex> lock(g_text_button_action_props_mutex);
			const std::string           prefix = std::string(prop_name) + "#";
			for (auto it = g_text_button_action_props.begin();
				 it != g_text_button_action_props.end();) {
				if (it->first != binding_id &&
					it->first.compare(0, prefix.size(), prefix) == 0) {
					it = g_text_button_action_props.erase(it);
				} else {
					++it;
				}
			}
			g_text_button_action_props[binding_id] = action_props;
		}

		if (max_input_chars < 0)
			max_input_chars = 0;

		std::string payload =
			std::string("TEXTBTN|") +
			escape_field(setting_key) + "|" +
			escape_field(binding_id.c_str()) + "|" +
			(input_enabled ? "1" : "0") + "|" +
			std::to_string(max_input_chars) + "|" +
			escape_field(label ? label : "") + "|" +
			std::to_string(button_count);
		for (size_t i = 0; i < button_count; ++i) {
			const auto &spec = buttons[i];
			payload += "|";
			payload += escape_field(spec.button_label);
			payload += "|";
			payload += (spec.enabled ? "1" : "0");
		}
		return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
	}

	obs_property_t *obs_properties_add_text_button(obs_properties_t      *props,
												   const char            *prop_name,
												   const char            *label,
												   const char            *setting_key,
												   const char            *button_label,
												   obs_property_clicked_t clicked,
												   void                  *clicked_priv,
												   bool                   input_enabled,
												   bool                   button_enabled,
												   int                    max_input_chars) {
		const ObsTextButtonActionSpec button = {
			button_label,
			clicked,
			clicked_priv,
			button_enabled,
		};
		return obs_properties_add_text_buttons(
			props,
			prop_name,
			label,
			setting_key,
			&button,
			1,
			input_enabled,
			max_input_chars);
	}

	void schedule_text_button_inject(obs_source_t *source) {
		if (!source)
			return;
		auto ctx = std::make_unique<TextButtonInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_text_button_inject, ctx.release(), false);
	}

} // namespace ods::widgets
