#include "widgets/pulldown-row-widget.hpp"

#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
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

		constexpr char kPulldownRowMagicPipe[]    = "PULLROW|";
		constexpr int  kPulldownRowInjectRetryMax = 40;
		constexpr int  kPulldownRowInjectRetryMs  = 5;

		// プレースホルダー payload を解析した 1 項目分の定義。
		struct ParsedPulldownOption {
			QString label;
			int64_t value = 0;
		};

		// プレースホルダー payload を解析した 1 プルダウン分の定義。
		struct ParsedPulldownSpec {
			QString                           setting_key;
			QString                           label;
			bool                              enabled       = true;
			bool                              store_as_bool = false;
			std::vector<ParsedPulldownOption> options;
		};

		// binding_id ごとに隠し list プロパティ群を引けるように保持する。
		std::unordered_map<std::string, std::vector<obs_property_t *>> g_pulldown_list_props;
		std::mutex                                                     g_pulldown_list_props_mutex;
		std::atomic<uint64_t>                                          g_pulldown_binding_seq{1};

		/// 隠し list 群を引くための一意 binding_id を生成する。
		std::string make_pulldown_binding_id(const char *prop_name) {
			std::string    base = prop_name ? prop_name : "";
			const uint64_t seq =
				g_pulldown_binding_seq.fetch_add(1, std::memory_order_relaxed);
			char suffix[32];
			std::snprintf(suffix, sizeof(suffix), "#%llu", static_cast<unsigned long long>(seq));
			return base + suffix;
		}

		/// "1"/それ以外を bool に変換する。
		bool parse_bool_field(const QString &value) {
			return value == QLatin1String("1");
		}

		// 複数プルダウンを 1 行で提供する inject 後ウィジェット。
		class PulldownRow : public QWidget {
		public:

			PulldownRow(obs_source_t *source, const char *binding_id, const std::vector<ParsedPulldownSpec> &pulldowns, QWidget *parent = nullptr)
				: QWidget(parent),
				  source_(source ? obs_source_get_ref(source) : nullptr),
				  binding_id_(binding_id ? binding_id : "") {
				auto *lay = new QHBoxLayout(this);
				lay->setContentsMargins(0, 0, 0, 0);
				lay->setSpacing(6);

				for (size_t i = 0; i < pulldowns.size(); ++i) {
					const auto &spec = pulldowns[i];
					if (!spec.label.isEmpty()) {
						auto *label_widget = new QLabel(spec.label);
						lay->addWidget(label_widget);
					}

					auto *combo = new QComboBox();
					combo->setEnabled(spec.enabled);
					for (const auto &opt : spec.options) {
						combo->addItem(opt.label, static_cast<qlonglong>(opt.value));
					}
					lay->addWidget(combo);

					const QString setting_key = spec.setting_key;
					connect(combo,
							QOverload<int>::of(&QComboBox::currentIndexChanged),
							this,
							[this, i](int) { onComboChanged(i); });

					combos_.push_back(combo);
					setting_keys_.push_back(setting_key);
					store_as_bools_.push_back(spec.store_as_bool);
				}

				// 右側の余白へストレッチを入れて、プルダウン群を左寄せにする。
				lay->addStretch(1);
				loadFromSettings();
				updateComboWidths();
				QTimer::singleShot(0, this, [this]() { updateComboWidths(); });
			}

			~PulldownRow() override {
				// binding_id のマップ削除はここでは行わない。
				// RefreshProperties 後の再 inject で同じ binding_id が必要になるため、
				// 古いエントリは次回の obs_properties_add_pulldown_row で掃除される。
				if (source_) {
					obs_source_release(source_);
					source_ = nullptr;
				}
			}

		private:

			/// フォント/スタイル変更時にコンボ幅を再計算する。
			bool event(QEvent *e) override {
				switch (e->type()) {
				case QEvent::FontChange:
				case QEvent::StyleChange:
				case QEvent::Polish:
					updateComboWidths();
					break;
				default:
					break;
				}
				return QWidget::event(e);
			}

			/// コンボ内の最長項目に合わせて最小幅を更新する。
			void updateComboWidths() {
				for (auto *combo : combos_) {
					if (!combo)
						continue;
					const QFontMetrics fm(combo->font());
					int                max_text_width = 0;
					for (int i = 0; i < combo->count(); ++i) {
						max_text_width = std::max(max_text_width,
												  fm.horizontalAdvance(combo->itemText(i)));
					}
					constexpr int kComboChromePx = 42;
					combo->setMinimumWidth(max_text_width + kComboChromePx);
				}
			}

			/// 設定値を読み込み、各プルダウンの選択へ反映する。
			void loadFromSettings() {
				if (!source_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s)
					return;

				for (size_t i = 0;
					 i < combos_.size() && i < setting_keys_.size() &&
					 i < store_as_bools_.size();
					 ++i) {
					auto *combo = combos_[i];
					if (!combo)
						continue;

					const std::string key = setting_keys_[i].toUtf8().constData();
					const int64_t     v   = store_as_bools_[i]
												? (obs_data_get_bool(s, key.c_str()) ? 1 : 0)
												: obs_data_get_int(s, key.c_str());
					int               idx = -1;
					for (int j = 0; j < combo->count(); ++j) {
						if (combo->itemData(j).toLongLong() == static_cast<qlonglong>(v)) {
							idx = j;
							break;
						}
					}
					if (idx >= 0) {
						combo->blockSignals(true);
						combo->setCurrentIndex(idx);
						combo->blockSignals(false);
					}
				}
				obs_data_release(s);
			}

			/// 設定同期後に隠し list modified コールバックを実行する。
			void onComboChanged(size_t index) {
				if (!source_ || index >= combos_.size() || index >= setting_keys_.size() ||
					index >= store_as_bools_.size())
					return;

				obs_property_t *list_prop = nullptr;
				{
					std::lock_guard<std::mutex> lock(g_pulldown_list_props_mutex);
					auto                        it = g_pulldown_list_props.find(binding_id_);
					if (it != g_pulldown_list_props.end() && index < it->second.size()) {
						list_prop = it->second[index];
					}
				}
				if (!list_prop)
					return;

				obs_data_t *s = obs_source_get_settings(source_);
				if (!s)
					return;

				const std::string key = setting_keys_[index].toUtf8().constData();
				const int64_t     v   = combos_[index]->currentData().toLongLong();
				if (store_as_bools_[index]) {
					obs_data_set_bool(s, key.c_str(), v != 0);
				} else {
					obs_data_set_int(s, key.c_str(), v);
				}

				const bool need_refresh = obs_property_modified(list_prop, s);
				obs_source_update(source_, s);
				obs_data_release(s);

				if (need_refresh && source_) {
					obs_source_update_properties(source_);
				}
			}

			obs_source_t            *source_ = nullptr;
			std::string              binding_id_;
			std::vector<QComboBox *> combos_;
			std::vector<QString>     setting_keys_;
			std::vector<bool>        store_as_bools_;
		};

		// inject 再試行状態を保持する UI タスク用コンテキスト。
		struct PulldownInjectCtx {
			explicit PulldownInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {
			}
			~PulldownInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kPulldownRowInjectRetryMax;
		};

		/// プルダウン行プレースホルダー payload を分解する。
		bool parse_pulldown_payload(const QString                   &text,
									QString                         &binding_id,
									QString                         &row_label,
									std::vector<ParsedPulldownSpec> &pulldowns) {
			QStringList fields;
			if (!split_escaped_pipe_fields(text, fields))
				return false;
			if (fields.size() < 4 || fields[0] != QLatin1String("PULLROW"))
				return false;

			bool      ok    = false;
			const int count = fields[3].toInt(&ok);
			if (!ok || count <= 0)
				return false;

			binding_id = fields[1];
			row_label  = fields[2];
			pulldowns.clear();
			pulldowns.reserve(static_cast<size_t>(count));

			int idx = 4;
			for (int i = 0; i < count; ++i) {
				if (idx + 5 > fields.size())
					return false;

				ParsedPulldownSpec spec;
				spec.setting_key   = fields[idx++];
				spec.label         = fields[idx++];
				spec.enabled       = parse_bool_field(fields[idx++]);
				spec.store_as_bool = parse_bool_field(fields[idx++]);

				bool      opt_count_ok = false;
				const int opt_count    = fields[idx++].toInt(&opt_count_ok);
				if (!opt_count_ok || opt_count <= 0)
					return false;

				spec.options.reserve(static_cast<size_t>(opt_count));
				for (int j = 0; j < opt_count; ++j) {
					if (idx + 2 > fields.size())
						return false;
					ParsedPulldownOption opt;
					opt.label           = fields[idx++];
					bool       value_ok = false;
					const auto parsed   = fields[idx++].toLongLong(&value_ok);
					if (!value_ok)
						return false;
					opt.value = static_cast<int64_t>(parsed);
					spec.options.push_back(opt);
				}
				pulldowns.push_back(std::move(spec));
			}

			return true;
		}

		/// プルダウン行プレースホルダーを実ウィジェットへ置換する。
		void do_pulldown_row_inject(void *param) {
			auto ctx = std::unique_ptr<PulldownInjectCtx>(static_cast<PulldownInjectCtx *>(param));
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
				if (text.startsWith(QLatin1String(kPulldownRowMagicPipe)))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found)
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

			int replaced_count = 0;
			for (const auto &ph : found) {
				QString                         binding_id;
				QString                         row_label;
				std::vector<ParsedPulldownSpec> pulldowns;
				if (!parse_pulldown_payload(ph.text, binding_id, row_label, pulldowns))
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

				auto *row_widget = new PulldownRow(
					ctx->source,
					binding_id.toUtf8().constData(),
					pulldowns,
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
				// UI ツリー反映待ちの取りこぼしを吸収するため再試行する。
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kPulldownRowInjectRetryMs,
								   [next]() { do_pulldown_row_inject(next); });
				return;
			}
		}

	} // namespace

	obs_property_t *obs_properties_add_pulldown_row(
		obs_properties_t      *props,
		const char            *prop_name,
		const char            *label,
		const ObsPulldownSpec *pulldowns,
		size_t                 pulldown_count) {
		if (!props || !prop_name || !*prop_name || !pulldowns || pulldown_count == 0)
			return nullptr;
		if (obs_properties_get(props, prop_name))
			return nullptr;

		std::unordered_set<std::string> list_names;
		list_names.reserve(pulldown_count);
		for (size_t i = 0; i < pulldown_count; ++i) {
			const auto &spec = pulldowns[i];
			if (!spec.list_prop_name || !*spec.list_prop_name || !spec.options ||
				spec.option_count == 0) {
				return nullptr;
			}
			const std::string list_name(spec.list_prop_name);
			if (list_name == prop_name)
				return nullptr;
			if (obs_properties_get(props, list_name.c_str()))
				return nullptr;
			if (!list_names.insert(list_name).second)
				return nullptr;

			for (size_t j = 0; j < spec.option_count; ++j) {
				const auto &opt = spec.options[j];
				if (!opt.item_label || !*opt.item_label)
					return nullptr;
			}
		}

		std::vector<obs_property_t *> list_props;
		list_props.reserve(pulldown_count);
		for (size_t i = 0; i < pulldown_count; ++i) {
			const auto     &spec      = pulldowns[i];
			obs_property_t *list_prop = obs_properties_add_list(
				props,
				spec.list_prop_name,
				spec.pulldown_label ? spec.pulldown_label : "",
				OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_INT);
			if (!list_prop)
				return nullptr;
			for (size_t j = 0; j < spec.option_count; ++j) {
				const auto &opt = spec.options[j];
				obs_property_list_add_int(list_prop, opt.item_label, opt.item_value);
			}
			if (spec.modified) {
				obs_property_set_modified_callback2(list_prop, spec.modified, spec.modified_priv);
			}
			obs_property_set_visible(list_prop, false);
			obs_property_set_enabled(list_prop, spec.enabled);
			list_props.push_back(list_prop);
		}

		const std::string binding_id = make_pulldown_binding_id(prop_name);

		std::string payload = "PULLROW|";
		payload += escape_field(binding_id.c_str());
		payload += "|";
		payload += escape_field(label ? label : "");
		payload += "|";
		payload += std::to_string(pulldown_count);

		for (size_t i = 0; i < pulldown_count; ++i) {
			const auto &spec = pulldowns[i];
			payload += "|";
			payload += escape_field(spec.list_prop_name);
			payload += "|";
			payload += escape_field(spec.pulldown_label ? spec.pulldown_label : "");
			payload += "|";
			payload += (spec.enabled ? "1" : "0");
			payload += "|";
			payload += (spec.store_as_bool ? "1" : "0");
			payload += "|";
			payload += std::to_string(spec.option_count);
			for (size_t j = 0; j < spec.option_count; ++j) {
				const auto &opt = spec.options[j];
				payload += "|";
				payload += escape_field(opt.item_label);
				payload += "|";
				payload += std::to_string(opt.item_value);
			}
		}

		obs_property_t *placeholder =
			obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
		if (!placeholder)
			return nullptr;

		{
			std::lock_guard<std::mutex> lock(g_pulldown_list_props_mutex);
			const std::string           prefix = std::string(prop_name) + "#";
			for (auto it = g_pulldown_list_props.begin();
				 it != g_pulldown_list_props.end();) {
				if (it->first != binding_id &&
					it->first.compare(0, prefix.size(), prefix) == 0) {
					it = g_pulldown_list_props.erase(it);
				} else {
					++it;
				}
			}
			g_pulldown_list_props[binding_id] = list_props;
		}
		return placeholder;
	}

	void schedule_pulldown_row_inject(obs_source_t *source) {
		if (!source)
			return;
		auto ctx = std::make_unique<PulldownInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_pulldown_row_inject, ctx.release(), false);
	}

} // namespace ods::widgets
