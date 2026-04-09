#include "widgets/mode-text-row-widget.hpp"

#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSizePolicy>
#include <QStringList>
#include <QTimer>
#include <QVariant>
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

		constexpr char kModeTextRowMagicPipe[]    = "MODETEXT|";
		constexpr int  kModeTextRowInjectRetryMax = 40;
		constexpr int  kModeTextRowInjectRetryMs  = 5;
		constexpr int  kComboChromePx             = 42;

		struct HiddenModeTextProps {
			obs_property_t *mode_prop = nullptr;
			obs_property_t *text_prop = nullptr;
		};

		struct ParsedModeTextOption {
			QString label;
			int64_t value = 0;
		};

		struct ParsedModeTextPayload {
			QString                           mode_setting_key;
			QString                           text_setting_key;
			QString                           binding_id;
			QString                           row_label;
			int64_t                           manual_mode_value  = 0;
			bool                              mode_store_as_bool = false;
			bool                              text_enabled       = true;
			int                               max_input_chars    = 0;
			std::vector<ParsedModeTextOption> options;
		};

		std::unordered_map<std::string, HiddenModeTextProps> g_mode_text_props;
		std::mutex                                           g_mode_text_props_mutex;
		std::atomic<uint64_t>                                g_mode_text_binding_seq{1};

		std::string make_mode_text_binding_id(const char *prop_name) {
			const uint64_t seq = g_mode_text_binding_seq.fetch_add(1, std::memory_order_relaxed);
			char           suffix[32];
			std::snprintf(suffix, sizeof(suffix), "#%llu", static_cast<unsigned long long>(seq));
			return std::string(prop_name ? prop_name : "") + suffix;
		}

		bool parse_bool_field(const QString &value) {
			return value == QLatin1String("1");
		}

		bool parse_mode_text_payload(const QString &text, ParsedModeTextPayload &payload) {
			QStringList fields;
			if (!split_escaped_pipe_fields(text, fields))
				return false;
			if (fields.size() < 10 || fields[0] != QLatin1String("MODETEXT"))
				return false;

			payload.mode_setting_key   = fields[1];
			payload.text_setting_key   = fields[2];
			payload.binding_id         = fields[3];
			payload.row_label          = fields[4];
			payload.manual_mode_value  = fields[5].toLongLong();
			payload.mode_store_as_bool = parse_bool_field(fields[6]);
			payload.text_enabled       = parse_bool_field(fields[7]);
			payload.max_input_chars    = fields[8].toInt();

			bool      count_ok     = false;
			const int option_count = fields[9].toInt(&count_ok);
			if (!count_ok || option_count <= 0)
				return false;

			int idx = 10;
			payload.options.clear();
			payload.options.reserve(static_cast<size_t>(option_count));
			for (int i = 0; i < option_count; ++i) {
				if (idx + 2 > fields.size())
					return false;
				ParsedModeTextOption opt;
				opt.label               = fields[idx++];
				bool       value_ok     = false;
				const auto parsed_value = fields[idx++].toLongLong(&value_ok);
				if (!value_ok)
					return false;
				opt.value = static_cast<int64_t>(parsed_value);
				payload.options.push_back(std::move(opt));
			}

			return !payload.mode_setting_key.isEmpty() &&
				   !payload.text_setting_key.isEmpty() &&
				   !payload.binding_id.isEmpty();
		}

		class ModeTextRow : public QWidget {
		public:

			ModeTextRow(obs_source_t                *source,
						const ParsedModeTextPayload &payload,
						QWidget                     *parent = nullptr)
				: QWidget(parent),
				  source_(source ? obs_source_get_ref(source) : nullptr),
				  mode_setting_key_(payload.mode_setting_key),
				  text_setting_key_(payload.text_setting_key),
				  binding_id_(payload.binding_id),
				  manual_mode_value_(payload.manual_mode_value),
				  mode_store_as_bool_(payload.mode_store_as_bool),
				  text_enabled_(payload.text_enabled) {
				auto *lay = new QHBoxLayout(this);
				lay->setContentsMargins(0, 0, 0, 0);
				lay->setSpacing(6);
				lay->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

				mode_combo_ = new QComboBox(this);
				for (const auto &opt : payload.options) {
					mode_combo_->addItem(opt.label, static_cast<qlonglong>(opt.value));
				}
				mode_combo_->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
				mode_combo_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
				lay->addWidget(mode_combo_, 0);

				edit_ = new QLineEdit(this);
				edit_->setMinimumWidth(0);
				edit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
				if (payload.max_input_chars > 0) {
					edit_->setMaxLength(payload.max_input_chars);
				}
				lay->addWidget(edit_, 1);

				connect(mode_combo_,
						QOverload<int>::of(&QComboBox::currentIndexChanged),
						this,
						[this](int) { on_mode_changed(); });
				connect(edit_, &QLineEdit::editingFinished, this, &ModeTextRow::on_editing_finished);

				load_from_settings();
				update_combo_width();
				QTimer::singleShot(0, this, [this]() { update_combo_width(); });
			}

			~ModeTextRow() override {
				{
					std::lock_guard<std::mutex> lock(g_mode_text_props_mutex);
					g_mode_text_props.erase(binding_id_.toStdString());
				}
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
					update_combo_width();
					break;
				default:
					break;
				}
				return QWidget::event(e);
			}

			void update_combo_width() {
				if (!mode_combo_)
					return;
				const QFontMetrics fm(mode_combo_->font());
				int                max_text_width = 0;
				for (int i = 0; i < mode_combo_->count(); ++i) {
					max_text_width = std::max(
						max_text_width,
						fm.horizontalAdvance(mode_combo_->itemText(i)));
				}
				mode_combo_->setFixedWidth(max_text_width + kComboChromePx);
			}

			void load_from_settings() {
				if (!source_ || !mode_combo_ || !edit_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s)
					return;

				const std::string mode_key = mode_setting_key_.toUtf8().constData();
				const std::string text_key = text_setting_key_.toUtf8().constData();
				current_mode_              = mode_store_as_bool_
												 ? (obs_data_get_bool(s, mode_key.c_str()) ? 1 : 0)
												 : obs_data_get_int(s, mode_key.c_str());
				manual_text_               = QString::fromUtf8(obs_data_get_string(s, text_key.c_str()));
				obs_data_release(s);

				int combo_index = mode_combo_->findData(static_cast<qlonglong>(current_mode_));
				if (combo_index < 0)
					combo_index = 0;

				mode_combo_->blockSignals(true);
				mode_combo_->setCurrentIndex(combo_index);
				mode_combo_->blockSignals(false);
				current_mode_ = mode_combo_->currentData().toLongLong();

				edit_->blockSignals(true);
				edit_->setText(manual_text_);
				edit_->blockSignals(false);
				apply_mode_enabled();
			}

			void apply_mode_enabled() {
				if (!edit_)
					return;
				const bool is_manual_mode = (current_mode_ == manual_mode_value_);
				const bool enabled        = text_enabled_ && is_manual_mode;
				edit_->setEnabled(enabled);
				edit_->setReadOnly(!enabled);
			}

			HiddenModeTextProps get_hidden_props() {
				std::lock_guard<std::mutex> lock(g_mode_text_props_mutex);
				auto                        it = g_mode_text_props.find(binding_id_.toStdString());
				if (it == g_mode_text_props.end())
					return {};
				return it->second;
			}

			void on_mode_changed() {
				if (!source_ || !mode_combo_)
					return;
				current_mode_ = mode_combo_->currentData().toLongLong();

				const HiddenModeTextProps hidden = get_hidden_props();
				if (!hidden.mode_prop)
					return;

				obs_source_t *source = obs_source_get_ref(source_);
				if (!source)
					return;

				obs_data_t *s = obs_source_get_settings(source);
				if (!s) {
					obs_source_release(source);
					return;
				}

				const std::string mode_key = mode_setting_key_.toUtf8().constData();
				const std::string text_key = text_setting_key_.toUtf8().constData();
				if (mode_store_as_bool_) {
					obs_data_set_bool(s, mode_key.c_str(), current_mode_ != 0);
				} else {
					obs_data_set_int(s, mode_key.c_str(), current_mode_);
				}

				QPointer<ModeTextRow> self(this);
				const bool            need_refresh = obs_property_modified(hidden.mode_prop, s);
				if (!self) {
					obs_data_release(s);
					obs_source_release(source);
					return;
				}

				const char   *latest_text    = obs_data_get_string(s, text_key.c_str());
				const QString refreshed_text = QString::fromUtf8(latest_text ? latest_text : "");
				obs_source_update(source, s);
				obs_data_release(s);
				if (!self) {
					obs_source_release(source);
					return;
				}

				if (need_refresh) {
					if (obs_source_t *refresh_source = obs_source_get_ref(source)) {
						obs_queue_task(OBS_TASK_UI, [](void *param) {
							auto *queued_source = static_cast<obs_source_t *>(param);
							obs_source_update_properties(queued_source);
							obs_source_release(queued_source); }, refresh_source, false);
					}
					obs_source_release(source);
					return;
				}
				manual_text_ = refreshed_text;
				edit_->blockSignals(true);
				edit_->setText(manual_text_);
				edit_->blockSignals(false);
				apply_mode_enabled();
				obs_source_release(source);
			}

			void on_editing_finished() {
				if (!source_ || !edit_)
					return;
				if (current_mode_ != manual_mode_value_ || !text_enabled_)
					return;

				const HiddenModeTextProps hidden = get_hidden_props();
				obs_source_t             *source = obs_source_get_ref(source_);
				if (!source)
					return;
				obs_data_t *s = obs_source_get_settings(source);
				if (!s) {
					obs_source_release(source);
					return;
				}

				manual_text_               = edit_->text();
				const QByteArray  utf8     = manual_text_.toUtf8();
				const std::string text_key = text_setting_key_.toUtf8().constData();
				obs_data_set_string(s, text_key.c_str(), utf8.constData());

				QPointer<ModeTextRow> self(this);
				const bool            need_refresh =
					hidden.text_prop ? obs_property_modified(hidden.text_prop, s) : false;
				if (!self) {
					obs_data_release(s);
					obs_source_release(source);
					return;
				}
				obs_source_update(source, s);
				obs_data_release(s);
				if (!self) {
					obs_source_release(source);
					return;
				}

				if (need_refresh) {
					if (obs_source_t *refresh_source = obs_source_get_ref(source)) {
						obs_queue_task(OBS_TASK_UI, [](void *param) {
							auto *queued_source = static_cast<obs_source_t *>(param);
							obs_source_update_properties(queued_source);
							obs_source_release(queued_source); }, refresh_source, false);
					}
				}
				obs_source_release(source);
			}

			obs_source_t *source_ = nullptr;
			QString       mode_setting_key_;
			QString       text_setting_key_;
			QString       binding_id_;
			int64_t       manual_mode_value_  = 0;
			bool          mode_store_as_bool_ = false;
			bool          text_enabled_       = true;
			int64_t       current_mode_       = 0;
			QString       manual_text_;

			QComboBox *mode_combo_ = nullptr;
			QLineEdit *edit_       = nullptr;
		};

		struct ModeTextInjectCtx {
			explicit ModeTextInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {
			}
			~ModeTextInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kModeTextRowInjectRetryMax;
		};

		void do_mode_text_row_inject(void *param) {
			auto ctx = std::unique_ptr<ModeTextInjectCtx>(static_cast<ModeTextInjectCtx *>(param));
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
				if (text.startsWith(QLatin1String(kModeTextRowMagicPipe))) {
					found.push_back({lbl, text});
				}
			}
			for (const auto &ph : found) {
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);
			}

			int replaced_count = 0;
			for (const auto &ph : found) {
				ParsedModeTextPayload payload;
				if (!parse_mode_text_payload(ph.text, payload))
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

				auto *row_widget = new ModeTextRow(ctx->source, payload, parent);
				form->removeRow(row);
				if (!payload.row_label.isEmpty()) {
					form->insertRow(row, payload.row_label, row_widget);
				} else {
					form->insertRow(row, row_widget);
				}
				++replaced_count;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kModeTextRowInjectRetryMs, [next]() { do_mode_text_row_inject(next); });
				return;
			}
		}

	} // namespace

	obs_property_t *obs_properties_add_mode_text_row(obs_properties_t         *props,
													 const char               *prop_name,
													 const char               *label,
													 const ObsModeTextRowSpec &spec) {
		if (!props || !prop_name || !*prop_name || !spec.mode_setting_key || !*spec.mode_setting_key ||
			!spec.text_setting_key || !*spec.text_setting_key || !spec.options || spec.option_count == 0) {
			return nullptr;
		}
		if (obs_properties_get(props, prop_name) ||
			obs_properties_get(props, spec.mode_setting_key) ||
			obs_properties_get(props, spec.text_setting_key)) {
			return nullptr;
		}

		std::unordered_set<int64_t> option_values;
		option_values.reserve(spec.option_count);
		for (size_t i = 0; i < spec.option_count; ++i) {
			const auto &opt = spec.options[i];
			if (!opt.item_label || !*opt.item_label)
				return nullptr;
			if (!option_values.insert(opt.item_value).second)
				return nullptr;
		}

		obs_property_t *mode_prop = obs_properties_add_list(
			props,
			spec.mode_setting_key,
			spec.mode_setting_key,
			OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
		if (!mode_prop)
			return nullptr;
		for (size_t i = 0; i < spec.option_count; ++i) {
			const auto &opt = spec.options[i];
			obs_property_list_add_int(mode_prop, opt.item_label, opt.item_value);
		}
		if (spec.mode_modified) {
			obs_property_set_modified_callback2(mode_prop, spec.mode_modified, spec.mode_modified_priv);
		}
		obs_property_set_visible(mode_prop, false);

		obs_property_t *text_prop = obs_properties_add_text(
			props,
			spec.text_setting_key,
			spec.text_setting_key,
			OBS_TEXT_DEFAULT);
		if (!text_prop)
			return nullptr;
		if (spec.text_modified) {
			obs_property_set_modified_callback2(text_prop, spec.text_modified, spec.text_modified_priv);
		}
		obs_property_set_visible(text_prop, false);
		obs_property_set_enabled(text_prop, spec.text_enabled);

		const std::string binding_id = make_mode_text_binding_id(prop_name);
		{
			std::lock_guard<std::mutex> lock(g_mode_text_props_mutex);
			g_mode_text_props[binding_id] = {mode_prop, text_prop};
		}

		int max_input_chars = spec.max_input_chars;
		if (max_input_chars < 0)
			max_input_chars = 0;

		std::string payload = std::string("MODETEXT|") +
							  escape_field(spec.mode_setting_key) + "|" +
							  escape_field(spec.text_setting_key) + "|" +
							  escape_field(binding_id.c_str()) + "|" +
							  escape_field(label ? label : "") + "|" +
							  std::to_string(spec.manual_mode_value) + "|" +
							  (spec.mode_store_as_bool ? "1" : "0") + "|" +
							  (spec.text_enabled ? "1" : "0") + "|" +
							  std::to_string(max_input_chars) + "|" +
							  std::to_string(spec.option_count);
		for (size_t i = 0; i < spec.option_count; ++i) {
			const auto &opt = spec.options[i];
			payload += "|";
			payload += escape_field(opt.item_label);
			payload += "|";
			payload += std::to_string(opt.item_value);
		}

		return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
	}

	void schedule_mode_text_row_inject(obs_source_t *source) {
		if (!source)
			return;
		auto ctx = std::make_unique<ModeTextInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_mode_text_row_inject, ctx.release(), false);
	}

} // namespace ods::widgets
