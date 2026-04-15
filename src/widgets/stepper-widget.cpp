#include "widgets/stepper-widget.hpp"

#include "widgets/focus-spin-box.hpp"
#include "widgets/widget-inject-utils.hpp"

#include <QApplication>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ods::widgets {

	namespace {

		constexpr char kStepperMagic[]        = "STEPPER\x1F"; // legacy format
		constexpr char kStepperMagicPipe[]    = "STEPPER|";    // current format
		constexpr int  kStepperInjectRetryMax = 40;
		constexpr int  kStepperInjectRetryMs  = 5;

		// OBS_TEXT_INFO プレースホルダーを StepperRow へ差し替える実体ウィジェット。
		class StepperRow : public QWidget {
		public:

			StepperRow(obs_source_t *source, const char *key, double min_val, double max_val, double def_val, int decimals, const char *suffix, bool store_as_int = false, int max_input_chars = 7, QWidget *parent = nullptr)
				: QWidget(parent),
				  source_(source ? obs_source_get_ref(source) : nullptr),
				  key_(key),
				  def_val_(def_val),
				  store_as_int_(store_as_int) {
				auto *lay = new QHBoxLayout(this);
				lay->setContentsMargins(0, 0, 0, 0);
				lay->setSpacing(4);
				lay->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

				reset_btn_ = new QPushButton(QStringLiteral("Reset"));
				reset_btn_->setStyleSheet(
					QStringLiteral("padding-left: 3px; padding-right: 3px;"));
				{
					const QFontMetrics fm(reset_btn_->font());
					const int          w = fm.horizontalAdvance(QStringLiteral("Reset")) + 20;
					reset_btn_->setFixedWidth(w);
				}
				lay->addWidget(reset_btn_);

				spin_ = new FocusSpinBox(this);
				spin_->setRange(min_val, max_val);
				spin_->setDecimals(decimals);
				if (suffix && *suffix)
					spin_->setSuffix(QString::fromUtf8(suffix));
				spin_->setSingleStep(1.0);
				spin_->setKeyboardTracking(false);
				spin_->setStyleSheet(QStringLiteral(
					"QAbstractSpinBox { padding-left: 4px; padding-right: 4px; }"));
				spin_->setMinimumWidth(0);
				spin_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
				{
					const int          clamped_chars = std::max(1, max_input_chars);
					const QFontMetrics fm(spin_->font());
					const QString      probe(clamped_chars, QLatin1Char('0'));
					const int          text_w = fm.horizontalAdvance(probe);
					const int          suffix_w =
						(suffix && *suffix) ? fm.horizontalAdvance(QString::fromUtf8(suffix)) : 0;
					constexpr int kSpinChromePx = 40; // up/down領域や枠、余白のぶん
					spin_->setMaximumWidth(text_w + suffix_w + kSpinChromePx);
				}
				lay->addWidget(spin_);

				static constexpr int kDeltas[] = {-100, -10, 10, 100};
				const int            delta_btn_w =
					QFontMetrics(font()).horizontalAdvance(QStringLiteral("+100")) + 8;
				for (int delta : kDeltas) {
					char label[8];
					std::snprintf(label, sizeof(label), "%+d", delta);
					auto *btn = new QPushButton(QString::fromUtf8(label));
					btn->setStyleSheet(
						QStringLiteral("padding-left: 3px; padding-right: 3px;"));
					btn->setFixedWidth(delta_btn_w);
					connect(btn, &QPushButton::clicked, this, [this, delta]() {
						spin_->setValue(spin_->value() + delta);
					});
					lay->addWidget(btn);
					delta_buttons_.push_back(btn);
				}

				connect(reset_btn_, &QPushButton::clicked, this, [this]() { spin_->setValue(def_val_); });

				connect(spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &StepperRow::onValueChanged);

				updateResetVisibility();
				loadFromSettings();
			}

			~StepperRow() override {
				if (source_) {
					obs_source_release(source_);
					source_ = nullptr;
				}
			}

			/// 現在設定値を読み込み、スピンボックスへ反映する。
			void loadFromSettings() {
				if (!source_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s)
					return;
				double v = store_as_int_
							   ? static_cast<double>(obs_data_get_int(s, key_.c_str()))
							   : obs_data_get_double(s, key_.c_str());
				obs_data_release(s);
				spin_->blockSignals(true);
				spin_->setValue(v);
				spin_->blockSignals(false);
			}

		private:

			/// リサイズ時に Reset ボタンの表示可否を再判定する。
			void resizeEvent(QResizeEvent *event) override {
				QWidget::resizeEvent(event);
				updateResetVisibility();
			}

			/// 横幅に応じて Reset ボタン表示を切り替える。
			void updateResetVisibility() {
				if (!reset_btn_)
					return;
				auto *lay = qobject_cast<QHBoxLayout *>(layout());
				if (!lay)
					return;

				constexpr int kSpinMinWidthWithReset = 90;
				const int     margins =
					lay->contentsMargins().left() + lay->contentsMargins().right();
				const int spacing     = lay->spacing();
				const int delta_count = static_cast<int>(delta_buttons_.size());

				int delta_total_w = 0;
				for (auto *b : delta_buttons_) {
					if (b)
						delta_total_w += b->width();
				}

				const int spin_w_if_reset_visible =
					width() - margins - delta_total_w - reset_btn_->width() -
					(spacing * (delta_count + 1));
				reset_btn_->setVisible(spin_w_if_reset_visible >=
									   kSpinMinWidthWithReset);
			}

			/// 値変更を OBS 設定へ書き戻す。
			void onValueChanged(double val) {
				if (!source_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (s) {
					if (store_as_int_) {
						obs_data_set_int(s, key_.c_str(), static_cast<int>(std::lround(val)));
					} else {
						obs_data_set_double(s, key_.c_str(), val);
					}
					obs_source_update(source_, s);
					obs_data_release(s);
				}
			}

			obs_source_t              *source_;
			std::string                key_;
			double                     def_val_;
			bool                       store_as_int_ = false;
			QPushButton               *reset_btn_    = nullptr;
			std::vector<QPushButton *> delta_buttons_;
			FocusSpinBox              *spin_ = nullptr;
		};

		// inject 再試行状態を保持する UI タスク用コンテキスト。
		struct StepperInjectCtx {
			explicit StepperInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {
			}
			~StepperInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kStepperInjectRetryMax;
		};

		/// 旧/新両フォーマットの Stepper payload を分解する。
		bool parse_stepper_payload(const QString &text, QStringList &fields) {
			if (text.startsWith(QLatin1String(kStepperMagicPipe))) {
				fields = text.split(QChar('|'));
				return fields.size() >= 8;
			}
			if (text.startsWith(QLatin1String(kStepperMagic))) {
				// 旧 payload 互換（区切り文字 0x1F）
				fields = text.split(QChar('\x1F'));
				return fields.size() >= 8;
			}
			return false;
		}

		/// Stepper プレースホルダーを実ウィジェットへ差し替える。
		void do_stepper_inject(void *param) {
			auto ctx = std::unique_ptr<StepperInjectCtx>(static_cast<StepperInjectCtx *>(param));
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
				if (text.startsWith(QLatin1String("STEPPER")))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found)
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

			int replaced_count = 0;
			for (auto &ph : found) {
				QStringList fields;
				if (!parse_stepper_payload(ph.text, fields))
					continue;

				const QString key             = fields[1];
				const double  min_val         = fields[2].toDouble();
				const double  max_val         = fields[3].toDouble();
				const double  def_val         = fields[4].toDouble();
				const int     decimals        = fields[5].toInt();
				const QString suffix          = fields[6];
				bool          store_as_int    = false;
				int           max_input_chars = 7;
				QString       row_label;
				QString       label_color;
				QString       help_text;
				if (fields.size() >= 11) {
					bool      ok           = false;
					const int parsed_chars = fields[8].toInt(&ok);
					if (ok) {
						store_as_int    = (fields[7] == QLatin1String("1"));
						max_input_chars = std::max(1, parsed_chars);
						row_label       = fields[9];
						label_color     = fields[10];
						// fields[11] 以降は help_text（| を含む可能性があるため join で復元）
						if (fields.size() >= 12)
							help_text = fields.mid(11).join(QChar('|'));
					} else {
						store_as_int = (fields[7] == QLatin1String("1"));
						row_label    = fields.mid(8).join(QStringLiteral(" "));
					}
				} else if (fields.size() >= 10) {
					bool      ok           = false;
					const int parsed_chars = fields[8].toInt(&ok);
					if (ok) {
						store_as_int    = (fields[7] == QLatin1String("1"));
						max_input_chars = std::max(1, parsed_chars);
						row_label       = fields.mid(9).join(QStringLiteral(" "));
					} else {
						store_as_int = (fields[7] == QLatin1String("1"));
						row_label    = fields.mid(8).join(QStringLiteral(" "));
					}
				} else if (fields.size() >= 9) {
					store_as_int = (fields[7] == QLatin1String("1"));
					row_label    = fields.mid(8).join(QStringLiteral(" "));
				} else {
					row_label = fields.mid(7).join(QStringLiteral(" "));
				}

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

				auto *stepper = new StepperRow(
					ctx->source,
					key.toUtf8().constData(),
					min_val,
					max_val,
					def_val,
					decimals,
					suffix.toUtf8().constData(),
					store_as_int,
					max_input_chars,
					parent);

				// help_text がある場合はコンテナで StepperRow + ヘルプ吹き出しを包む
				QWidget *inject_widget = stepper;
				if (!help_text.isEmpty()) {
					auto *container = new QWidget(parent);
					auto *vlay      = new QVBoxLayout(container);
					vlay->setContentsMargins(0, 0, 0, 0);
					vlay->setSpacing(0);
					vlay->addWidget(stepper);
					vlay->addWidget(create_help_callout(help_text, container));
					inject_widget = container;
				}

				form->removeRow(row);
				if (!row_label.isEmpty()) {
					QColor color(label_color);
					if (color.isValid())
						form->insertRow(row, create_colored_label(row_label, color, parent), inject_widget);
					else
						form->insertRow(row, row_label, inject_widget);
				} else {
					form->insertRow(row, inject_widget);
				}
				++replaced_count;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				// プロパティ構築タイミング差を吸収するため再試行する。
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kStepperInjectRetryMs,
								   [next]() { do_stepper_inject(next); });
				return;
			}
		}

	} // namespace

	obs_property_t *obs_properties_add_stepper(obs_properties_t *props,
											   const char       *prop_name,
											   const char       *label,
											   const char       *setting_key,
											   double            min_val,
											   double            max_val,
											   double            def_val,
											   int               decimals,
											   const char       *suffix,
											   bool              store_as_int,
											   int               max_input_chars,
											   const char       *label_color,
											   const char       *help_text) {
		const int   clamped_chars = std::max(1, max_input_chars);
		const char *color_str     = (label_color && *label_color) ? label_color : "";
		const char *help_str      = (help_text && *help_text) ? help_text : "";
		const int   len           = std::snprintf(nullptr, 0, "STEPPER|%s|%.10g|%.10g|%.10g|%d|%s|%d|%d|%s|%s|%s", setting_key, min_val, max_val, def_val, decimals, suffix ? suffix : "", store_as_int ? 1 : 0, clamped_chars, label ? label : "", color_str, help_str);
		if (len < 0)
			return nullptr;

		std::string buf(static_cast<size_t>(len) + 1, '\0');
		std::snprintf(buf.data(), buf.size(), "STEPPER|%s|%.10g|%.10g|%.10g|%d|%s|%d|%d|%s|%s|%s", setting_key, min_val, max_val, def_val, decimals, suffix ? suffix : "", store_as_int ? 1 : 0, clamped_chars, label ? label : "", color_str, help_str);
		return obs_properties_add_text(props, prop_name, buf.c_str(), OBS_TEXT_INFO);
	}

	void schedule_stepper_inject(obs_source_t *source) {
		if (!source)
			return;
		auto ctx = std::make_unique<StepperInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_stepper_inject, ctx.release(), false);
	}

} // namespace ods::widgets
