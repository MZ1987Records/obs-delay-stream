#include "widgets/path-mode-row-widget.hpp"

#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QFontMetrics>
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

		constexpr char kPathModeMagicPipe[]    = "PATHMODE|";
		constexpr int  kPathModeInjectRetryMax = 40;
		constexpr int  kPathModeInjectRetryMs  = 5;
		constexpr int  kModeAuto               = 0;
		constexpr int  kModeFromPath           = 1;
		constexpr int  kModeAbsolute           = 2;

		std::unordered_map<std::string, obs_property_t *> g_path_mode_download_props;
		std::mutex                                        g_path_mode_download_props_mutex;
		std::atomic<uint64_t>                             g_path_mode_binding_seq{1};

		std::string make_path_mode_action_name(const char *prop_name) {
			std::string action_name = prop_name ? prop_name : "";
			action_name += "__pathmode_download_action";
			return action_name;
		}

		std::string make_path_mode_binding_id(const char *prop_name) {
			const uint64_t seq = g_path_mode_binding_seq.fetch_add(1, std::memory_order_relaxed);
			char           suffix[32];
			std::snprintf(suffix, sizeof(suffix), "#%llu", static_cast<unsigned long long>(seq));
			return std::string(prop_name ? prop_name : "") + suffix;
		}

		struct ParsedPathModePayload {
			QString mode_setting_key;
			QString path_setting_key;
			QString binding_id;
			QString row_label;
			bool    auto_path_exists = false;
			QString auto_path_display;
			bool    manual_enabled  = true;
			int     max_input_chars = 0;
			QString auto_mode_label;
			QString path_mode_label;
			QString absolute_mode_label;
			QString download_button_label;
			bool    download_enabled = true;
		};

		bool parse_bool_field(const QString &value) {
			return value == QLatin1String("1");
		}

		bool parse_path_mode_payload(const QString &text, ParsedPathModePayload &parsed) {
			QStringList fields;
			if (!split_escaped_pipe_fields(text, fields))
				return false;
			if (fields.size() < 14 || fields[0] != QLatin1String("PATHMODE"))
				return false;

			parsed.mode_setting_key      = fields[1];
			parsed.path_setting_key      = fields[2];
			parsed.binding_id            = fields[3];
			parsed.row_label             = fields[4];
			parsed.auto_path_exists      = parse_bool_field(fields[5]);
			parsed.auto_path_display     = fields[6];
			parsed.manual_enabled        = parse_bool_field(fields[7]);
			parsed.max_input_chars       = fields[8].toInt();
			parsed.auto_mode_label       = fields[9];
			parsed.path_mode_label       = fields[10];
			parsed.absolute_mode_label   = fields[11];
			parsed.download_button_label = fields[12];
			parsed.download_enabled      = parse_bool_field(fields[13]);

			return !parsed.mode_setting_key.isEmpty() && !parsed.path_setting_key.isEmpty();
		}

		class PathModeRow : public QWidget {
		public:

			PathModeRow(obs_source_t                *source,
						const ParsedPathModePayload &payload,
						QWidget                     *parent = nullptr)
				: QWidget(parent),
				  source_(source ? obs_source_get_ref(source) : nullptr),
				  mode_setting_key_(payload.mode_setting_key),
				  path_setting_key_(payload.path_setting_key),
				  binding_id_(payload.binding_id),
				  auto_path_exists_(payload.auto_path_exists),
				  auto_path_display_(payload.auto_path_display),
				  manual_enabled_(payload.manual_enabled),
				  download_enabled_(payload.download_enabled) {
				lay_ = new QHBoxLayout(this);
				lay_->setContentsMargins(0, 0, 0, 0);
				lay_->setSpacing(6);
				lay_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

				mode_combo_ = new QComboBox(this);
				mode_combo_->addItem(payload.auto_mode_label, kModeAuto);
				mode_combo_->addItem(payload.path_mode_label, kModeFromPath);
				mode_combo_->addItem(payload.absolute_mode_label, kModeAbsolute);
				mode_combo_->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
				mode_combo_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
				{
					const QFontMetrics fm(mode_combo_->font());
					int                max_text_width = 0;
					for (int i = 0; i < mode_combo_->count(); ++i) {
						max_text_width = std::max(
							max_text_width,
							fm.horizontalAdvance(mode_combo_->itemText(i)));
					}
					constexpr int kComboChromePx = 50;
					mode_combo_->setFixedWidth(max_text_width + kComboChromePx);
				}
				lay_->addWidget(mode_combo_, 0);
				lay_->setAlignment(mode_combo_, Qt::AlignLeft | Qt::AlignVCenter);

				edit_ = new QLineEdit(this);
				edit_->setMinimumWidth(0);
				edit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
				if (payload.max_input_chars > 0) {
					edit_->setMaxLength(payload.max_input_chars);
				}
				lay_->addWidget(edit_, 1);

				download_btn_ = new QPushButton(payload.download_button_label, this);
				download_btn_->setEnabled(download_enabled_);
				download_btn_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
				download_btn_->setFixedWidth(download_btn_->sizeHint().width());
				lay_->addWidget(download_btn_, 0);
				lay_->setAlignment(download_btn_, Qt::AlignLeft | Qt::AlignVCenter);

				connect(mode_combo_,
						QOverload<int>::of(&QComboBox::currentIndexChanged),
						this,
						[this](int) { onModeChanged(); });
				connect(edit_, &QLineEdit::editingFinished, this, &PathModeRow::onEditingFinished);
				connect(download_btn_, &QPushButton::clicked, this, &PathModeRow::onDownloadClicked);

				load_from_settings();
				QTimer::singleShot(0, this, [this]() { apply_mode_visibility(); });
			}

			~PathModeRow() override {
				// binding_id のマップ削除はここでは行わない。
				// RefreshProperties 後の再 inject で同じ binding_id が必要になるため、
				// 古いエントリは次回の obs_properties_add_path_mode_row で掃除される。
				if (source_) {
					obs_source_release(source_);
					source_ = nullptr;
				}
			}

		private:

			int current_mode() const {
				if (!mode_combo_)
					return kModeAuto;
				const int v = mode_combo_->currentData().toInt();
				if (v != kModeFromPath && v != kModeAbsolute)
					return kModeAuto;
				return v;
			}

			void load_from_settings() {
				if (!source_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s)
					return;

				const int mode_raw = static_cast<int>(obs_data_get_int(s, mode_setting_key_.toUtf8().constData()));
				mode_              = (mode_raw == kModeFromPath || mode_raw == kModeAbsolute) ? mode_raw : kModeAuto;
				manual_path_       = QString::fromUtf8(obs_data_get_string(s, path_setting_key_.toUtf8().constData()));
				obs_data_release(s);

				const int combo_index = mode_combo_ ? mode_combo_->findData(mode_) : -1;
				if (mode_combo_ && combo_index >= 0) {
					mode_combo_->blockSignals(true);
					mode_combo_->setCurrentIndex(combo_index);
					mode_combo_->blockSignals(false);
				}
				if (edit_) {
					edit_->blockSignals(true);
					edit_->setText(manual_path_);
					edit_->blockSignals(false);
				}
			}

			void sync_mode_to_settings() {
				if (!source_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s)
					return;
				obs_data_set_int(s, mode_setting_key_.toUtf8().constData(), mode_);
				obs_source_update(source_, s);
				obs_data_release(s);
			}

			void sync_manual_path_to_settings() {
				if (!source_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s)
					return;
				const QByteArray utf8 = manual_path_.toUtf8();
				obs_data_set_string(s, path_setting_key_.toUtf8().constData(), utf8.constData());
				obs_source_update(source_, s);
				obs_data_release(s);
			}

			void apply_mode_visibility() {
				if (!lay_ || !edit_ || !download_btn_)
					return;

				auto collapse_edit_slot = [this]() {
					lay_->setStretchFactor(edit_, 0);
					edit_->setMinimumWidth(0);
					edit_->setMaximumWidth(0);
					edit_->hide();
				};
				auto restore_edit_slot = [this]() {
					lay_->setStretchFactor(edit_, 1);
					edit_->setMinimumWidth(0);
					edit_->setMaximumWidth(QWIDGETSIZE_MAX);
					edit_->show();
				};

				edit_->hide();
				download_btn_->hide();

				if (mode_ == kModeAuto) {
					if (auto_path_exists_) {
						restore_edit_slot();
						apply_lineedit_readonly_look(edit_, true);
						edit_->setText(auto_path_display_);
					} else {
						collapse_edit_slot();
						download_btn_->setEnabled(download_enabled_);
						download_btn_->show();
					}
					return;
				}

				if (mode_ == kModeAbsolute) {
					restore_edit_slot();
					apply_lineedit_readonly_look(edit_, !manual_enabled_);
					edit_->setText(manual_path_);
				} else {
					collapse_edit_slot();
				}
			}

			void onModeChanged() {
				if (mode_ == kModeAbsolute && edit_) {
					manual_path_ = edit_->text();
				}
				mode_ = current_mode();
				sync_mode_to_settings();
				apply_mode_visibility();
			}

			void onEditingFinished() {
				if (!edit_ || mode_ != kModeAbsolute)
					return;
				manual_path_ = edit_->text();
				sync_manual_path_to_settings();
			}

			void onDownloadClicked() {
				obs_property_t *action_prop = nullptr;
				{
					std::lock_guard<std::mutex> lock(g_path_mode_download_props_mutex);
					auto                        it = g_path_mode_download_props.find(binding_id_.toStdString());
					if (it != g_path_mode_download_props.end()) {
						action_prop = it->second;
					}
				}
				if (!action_prop || !source_)
					return;

				const bool need_refresh = obs_property_button_clicked(action_prop, source_);
				if (need_refresh) {
					obs_source_update_properties(source_);
				}
			}

			obs_source_t *source_ = nullptr;
			QString       mode_setting_key_;
			QString       path_setting_key_;
			QString       binding_id_;
			int           mode_             = kModeAuto;
			bool          auto_path_exists_ = false;
			QString       auto_path_display_;
			QString       manual_path_;
			bool          manual_enabled_   = true;
			bool          download_enabled_ = true;
			QHBoxLayout  *lay_              = nullptr;

			QComboBox   *mode_combo_   = nullptr;
			QLineEdit   *edit_         = nullptr;
			QPushButton *download_btn_ = nullptr;
		};

		struct PathModeInjectCtx {
			explicit PathModeInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {
			}
			~PathModeInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kPathModeInjectRetryMax;
		};

		void do_path_mode_row_inject(void *param) {
			auto ctx = std::unique_ptr<PathModeInjectCtx>(static_cast<PathModeInjectCtx *>(param));
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
				if (text.startsWith(QLatin1String(kPathModeMagicPipe)))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found) {
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);
			}

			int replaced_count = 0;
			for (const auto &ph : found) {
				ParsedPathModePayload payload;
				if (!parse_path_mode_payload(ph.text, payload))
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

				auto *row_widget = new PathModeRow(ctx->source, payload, parent);
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
				QTimer::singleShot(kPathModeInjectRetryMs, [next]() { do_path_mode_row_inject(next); });
				return;
			}
		}

	} // namespace

	obs_property_t *obs_properties_add_path_mode_row(obs_properties_t         *props,
													 const char               *prop_name,
													 const char               *label,
													 const ObsPathModeRowSpec &spec) {
		if (!props || !prop_name || !*prop_name || !spec.mode_setting_key || !*spec.mode_setting_key ||
			!spec.path_setting_key || !*spec.path_setting_key) {
			return nullptr;
		}
		if (obs_properties_get(props, prop_name) ||
			obs_properties_get(props, spec.mode_setting_key) ||
			obs_properties_get(props, spec.path_setting_key)) {
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
		obs_property_list_add_int(mode_prop, spec.auto_mode_label ? spec.auto_mode_label : "Auto", kModeAuto);
		obs_property_list_add_int(mode_prop, spec.path_mode_label ? spec.path_mode_label : "From PATH", kModeFromPath);
		obs_property_list_add_int(mode_prop, spec.absolute_mode_label ? spec.absolute_mode_label : "Absolute path", kModeAbsolute);
		obs_property_set_visible(mode_prop, false);

		obs_property_t *path_prop = obs_properties_add_text(
			props,
			spec.path_setting_key,
			spec.path_setting_key,
			OBS_TEXT_DEFAULT);
		if (!path_prop)
			return nullptr;
		obs_property_set_visible(path_prop, false);
		obs_property_set_enabled(path_prop, spec.manual_enabled);

		obs_property_t *download_action_prop = nullptr;
		if (spec.download_clicked) {
			const std::string action_name = make_path_mode_action_name(prop_name);
			if (obs_properties_get(props, action_name.c_str()))
				return nullptr;
			download_action_prop = obs_properties_add_button2(
				props,
				action_name.c_str(),
				spec.download_button_label ? spec.download_button_label : "Download",
				spec.download_clicked,
				spec.download_clicked_priv);
			if (!download_action_prop)
				return nullptr;
			obs_property_set_visible(download_action_prop, false);
			obs_property_set_enabled(download_action_prop, spec.download_enabled);
		}

		const std::string binding_id = make_path_mode_binding_id(prop_name);
		if (download_action_prop) {
			std::lock_guard<std::mutex> lock(g_path_mode_download_props_mutex);
			const std::string           prefix = std::string(prop_name) + "#";
			for (auto it = g_path_mode_download_props.begin();
				 it != g_path_mode_download_props.end();) {
				if (it->first != binding_id &&
					it->first.compare(0, prefix.size(), prefix) == 0) {
					it = g_path_mode_download_props.erase(it);
				} else {
					++it;
				}
			}
			g_path_mode_download_props[binding_id] = download_action_prop;
		}

		int max_input_chars = spec.max_input_chars;
		if (max_input_chars < 0)
			max_input_chars = 0;

		std::string payload = std::string("PATHMODE|") +
							  escape_field(spec.mode_setting_key) + "|" +
							  escape_field(spec.path_setting_key) + "|" +
							  escape_field(binding_id.c_str()) + "|" +
							  escape_field(label ? label : "") + "|" +
							  (spec.auto_path_exists ? "1" : "0") + "|" +
							  escape_field(spec.auto_path_display ? spec.auto_path_display : "") + "|" +
							  (spec.manual_enabled ? "1" : "0") + "|" +
							  std::to_string(max_input_chars) + "|" +
							  escape_field(spec.auto_mode_label ? spec.auto_mode_label : "Auto") + "|" +
							  escape_field(spec.path_mode_label ? spec.path_mode_label : "From PATH") + "|" +
							  escape_field(spec.absolute_mode_label ? spec.absolute_mode_label : "Absolute path") + "|" +
							  escape_field(spec.download_button_label ? spec.download_button_label : "Download") + "|" +
							  (spec.download_enabled ? "1" : "0");

		return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
	}

	void schedule_path_mode_row_inject(obs_source_t *source) {
		if (!source)
			return;
		auto ctx = std::make_unique<PathModeInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_path_mode_row_inject, ctx.release(), false);
	}

} // namespace ods::widgets
