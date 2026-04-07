#include "widgets/delay-table-widget.hpp"

#include "widgets/focus-spin-box.hpp"
#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>
#include <string>
#include <vector>

namespace ods::widgets {

	namespace {

		constexpr char kDelayTableMagic[]        = "DTABLE|";
		constexpr int  kDelayTableInjectRetryMax = 40;
		constexpr int  kDelayTableInjectRetryMs  = 5;

		// ペイロードをパースしたチャンネル情報。
		struct ParsedChannel {
			QString name;
			double  measured_ms; // -1 if not measured
			double  base_ms;
			double  adjust_ms;
			double  global_ms;
			double  total_ms;
			bool    warn;
		};

		// ============================================================
		// DelayTableWidget
		// ============================================================
		class DelayTableWidget : public QWidget {
		public:

			DelayTableWidget(obs_source_t                     *source,
							 int                               selected_ch,
							 const std::vector<ParsedChannel> &channels,
							 const QStringList                &labels, // hdr_ch,name,measured,base,adjust,global,total,lbl_editor
							 QWidget                          *parent = nullptr)
				: QWidget(parent), source_(source ? obs_source_get_ref(source) : nullptr), ch_count_(static_cast<int>(channels.size())), selected_ch_(selected_ch >= 0 && selected_ch < static_cast<int>(channels.size()) ? selected_ch : 0) {
				auto *vlay = new QVBoxLayout(this);
				vlay->setContentsMargins(0, 2, 0, 2);
				vlay->setSpacing(6);

				// テーブル
				table_ = new QTableWidget(ch_count_, 7, this);
				table_->setHorizontalHeaderLabels({
					labels.value(0),
					labels.value(1),
					labels.value(2),
					labels.value(3),
					labels.value(4),
					labels.value(5),
					labels.value(6),
				});
				table_->setSelectionBehavior(QAbstractItemView::SelectRows);
				table_->setSelectionMode(QAbstractItemView::SingleSelection);
				table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
				table_->verticalHeader()->hide();
				table_->setShowGrid(true);
				table_->setAlternatingRowColors(true);
				table_->setFocusPolicy(Qt::NoFocus);
				table_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
				table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

				{
					auto *hdr = table_->horizontalHeader();
					hdr->setSectionResizeMode(0, QHeaderView::ResizeToContents); // Ch
					hdr->setSectionResizeMode(1, QHeaderView::Stretch);          // 名前
					hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents); // 計測
					hdr->setSectionResizeMode(3, QHeaderView::ResizeToContents); // ベース
					hdr->setSectionResizeMode(4, QHeaderView::ResizeToContents); // アジャスト
					hdr->setSectionResizeMode(5, QHeaderView::ResizeToContents); // 共通
					hdr->setSectionResizeMode(6, QHeaderView::ResizeToContents); // 合計
				}

				for (int i = 0; i < ch_count_; ++i) {
					const auto &ch = channels[i];

					auto makeItem = [](const QString &text,
									   Qt::Alignment  align = Qt::AlignRight | Qt::AlignVCenter) {
						auto *item = new QTableWidgetItem(text);
						item->setTextAlignment(align);
						return item;
					};

					table_->setItem(i, 0, makeItem(QString::number(i + 1)));
					table_->setItem(i, 1, makeItem(ch.name.isEmpty() ? QStringLiteral("—") : ch.name, Qt::AlignLeft | Qt::AlignVCenter));
					table_->setItem(i, 2, makeItem(ch.measured_ms < 0.0 ? QStringLiteral("—") : QString::number(ch.measured_ms, 'f', 1)));
					table_->setItem(i, 3, makeItem(QString::number(ch.base_ms, 'f', 1)));
					table_->setItem(i, 4, makeItem(QString::number(ch.adjust_ms, 'f', 0)));
					table_->setItem(i, 5, makeItem(QString::number(ch.global_ms, 'f', 0)));

					auto *total_item = makeItem(QString::number(ch.total_ms, 'f', 1));
					if (ch.warn) {
						total_item->setForeground(QColor(Qt::red));
						QFont f = total_item->font();
						f.setBold(true);
						total_item->setFont(f);
					}
					table_->setItem(i, 6, total_item);
				}

				// 行の上下パディングを詰めて固定する。
				table_->verticalHeader()->setDefaultSectionSize(22);
				{
					int h = table_->horizontalHeader()->height() + 4;
					for (int i = 0; i < ch_count_; ++i)
						h += table_->rowHeight(i);
					table_->setFixedHeight(h);
				}

				connect(table_, &QTableWidget::itemSelectionChanged, this, &DelayTableWidget::onSelectionChanged);

				vlay->addWidget(table_);

				// 選択チャンネルの「アジャスト」エディタ
				// 順序は StepperRow に合わせる: [ラベル] [Reset] [Spin] [デルタボタン群]
				auto *hlay = new QHBoxLayout();
				hlay->setContentsMargins(0, 0, 0, 0);
				hlay->setSpacing(4);
				hlay->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

				auto *adj_label = new QLabel(labels.value(7), this);
				adj_label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
				hlay->addWidget(adj_label);

				auto *reset_btn = new QPushButton(QStringLiteral("Reset"), this);
				reset_btn->setStyleSheet(
					QStringLiteral("padding-left: 3px; padding-right: 3px;"));
				{
					const int w = QFontMetrics(reset_btn->font())
									  .horizontalAdvance(QStringLiteral("Reset")) +
								  20;
					reset_btn->setFixedWidth(w);
				}
				connect(reset_btn, &QPushButton::clicked, this, [this]() { spin_->setValue(0.0); });
				hlay->addWidget(reset_btn);

				spin_ = new FocusSpinBox(this);
				spin_->setRange(-500.0, 500.0); // SUB_ADJUST_MIN_MS .. SUB_ADJUST_MAX_MS
				spin_->setDecimals(0);
				spin_->setSuffix(QStringLiteral(" ms"));
				spin_->setSingleStep(1.0);
				spin_->setKeyboardTracking(false);
				spin_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
				spin_->setStyleSheet(
					QStringLiteral("QAbstractSpinBox { padding-left: 4px; padding-right: 4px; }"));
				{
					constexpr int      kMaxInputChars = 7;
					const QFontMetrics fm(spin_->font());
					const QString      probe(kMaxInputChars, QLatin1Char('0'));
					const int          text_w        = fm.horizontalAdvance(probe);
					const int          suffix_w      = fm.horizontalAdvance(QStringLiteral(" ms"));
					constexpr int      kSpinChromePx = 40; // up/down領域や枠、余白のぶん
					spin_->setMaximumWidth(text_w + suffix_w + kSpinChromePx);
				}
				hlay->addWidget(spin_);

				const int delta_btn_w =
					QFontMetrics(font()).horizontalAdvance(QStringLiteral("+100")) + 8;
				for (int delta : {-100, -10, 10, 100}) {
					char lbl[8];
					std::snprintf(lbl, sizeof(lbl), "%+d", delta);
					auto *btn = new QPushButton(QString::fromUtf8(lbl), this);
					btn->setStyleSheet(
						QStringLiteral("padding-left: 3px; padding-right: 3px;"));
					btn->setFixedWidth(delta_btn_w);
					connect(btn, &QPushButton::clicked, this, [this, delta]() {
						spin_->setValue(spin_->value() + delta);
					});
					hlay->addWidget(btn);
				}

				vlay->addLayout(hlay);

				connect(spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &DelayTableWidget::onValueChanged);

				// 初期行を選択してスピンを同期する。
				table_->blockSignals(true);
				if (ch_count_ > 0)
					table_->selectRow(selected_ch_);
				table_->blockSignals(false);
				loadCurrentChannelValue();
			}

			~DelayTableWidget() override {
				if (source_) {
					obs_source_release(source_);
					source_ = nullptr;
				}
			}

		private:

			// 行選択が変わったとき: 選択チャンネルを保存してスピンを更新する。
			void onSelectionChanged() {
				const QList<QTableWidgetItem *> sel = table_->selectedItems();
				if (!sel.isEmpty()) {
					const int row = table_->row(sel.first());
					if (row >= 0 && row < ch_count_)
						selected_ch_ = row;
				}
				saveSelectedChannel();
				loadCurrentChannelValue();
			}

			// OBS設定から選択中チャンネルの adjust_ms を読んでスピンに反映する。
			void loadCurrentChannelValue() {
				if (!source_ || selected_ch_ < 0 || selected_ch_ >= ch_count_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s) return;
				char key[32];
				std::snprintf(key, sizeof(key), "sub%d_adjust_ms", selected_ch_);
				const double v = obs_data_get_double(s, key);
				obs_data_release(s);
				spin_->blockSignals(true);
				spin_->setValue(v);
				spin_->blockSignals(false);
			}

			// 選択チャンネルインデックスを OBS 設定に保存する。
			// delay_table_selected_ch は update に影響しないため UI 再構築は起きない。
			void saveSelectedChannel() {
				if (!source_) return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s) return;
				obs_data_set_int(s, "delay_table_selected_ch", static_cast<long long>(selected_ch_));
				obs_source_update(source_, s);
				obs_data_release(s);
			}

			// スピン値が変わったとき: OBS 設定の sub{N}_adjust_ms を更新する。
			// update が effective_delay_changed を検出してテーブルを再構築する。
			void onValueChanged(double val) {
				if (!source_ || selected_ch_ < 0 || selected_ch_ >= ch_count_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s) return;
				char key[32];
				std::snprintf(key, sizeof(key), "sub%d_adjust_ms", selected_ch_);
				obs_data_set_double(s, key, val);
				obs_source_update(source_, s);
				obs_data_release(s);
			}

			obs_source_t *source_      = nullptr;
			int           ch_count_    = 0;
			int           selected_ch_ = 0;
			QTableWidget *table_       = nullptr;
			FocusSpinBox *spin_        = nullptr;
		};

		// ============================================================
		// inject インフラ
		// ============================================================
		struct DelayTableInjectCtx {
			explicit DelayTableInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {
			}
			~DelayTableInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kDelayTableInjectRetryMax;
		};

		// ペイロード文字列をパースしてラベル・チャンネルリストを返す。
		// 書式: DTABLE|selected_ch|N|hdr_ch|hdr_name|hdr_measured|hdr_base|hdr_adjust|hdr_global|hdr_total|lbl_editor
		//            |ch0_name|ch0_measured|ch0_base|ch0_adjust|ch0_global|ch0_total|ch0_warn|...
		bool parse_delay_table_payload(const QString              &text,
									   int                        &selected_ch,
									   QStringList                &labels,
									   std::vector<ParsedChannel> &channels) {
			QStringList fields;
			if (!split_escaped_pipe_fields(text, fields))
				return false;
			if (fields.empty() || fields[0] != QLatin1String("DTABLE"))
				return false;
			if (fields.size() < 3)
				return false;

			bool ok     = false;
			selected_ch = fields[1].toInt(&ok);
			if (!ok) return false;

			const int n = fields[2].toInt(&ok);
			if (!ok || n <= 0) return false;

			// [3..10] = 8 label fields, [11..] = n*7 channel fields
			if (static_cast<int>(fields.size()) < 11 + n * 7)
				return false;

			labels = fields.mid(3, 8);

			channels.clear();
			channels.reserve(static_cast<size_t>(n));
			int idx = 11;
			for (int i = 0; i < n; ++i) {
				ParsedChannel ch;
				ch.name        = fields[idx++];
				ch.measured_ms = fields[idx++].toDouble();
				ch.base_ms     = fields[idx++].toDouble();
				ch.adjust_ms   = fields[idx++].toDouble();
				ch.global_ms   = fields[idx++].toDouble();
				ch.total_ms    = fields[idx++].toDouble();
				ch.warn        = (fields[idx++] == QLatin1String("1"));
				channels.push_back(ch);
			}
			return true;
		}

		/// DelayTable プレースホルダーを実テーブルウィジェットへ差し替える。
		void do_delay_table_inject(void *param) {
			auto *ctx = static_cast<DelayTableInjectCtx *>(param);
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
				if (text.startsWith(QLatin1String(kDelayTableMagic)))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found)
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

			int replaced_count = 0;
			for (const auto &ph : found) {
				int                        selected_ch = 0;
				QStringList                labels;
				std::vector<ParsedChannel> channels;
				if (!parse_delay_table_payload(ph.text, selected_ch, labels, channels))
					continue;

				QWidget *parent = ph.label->parentWidget();
				if (!parent) continue;
				auto *form = qobject_cast<QFormLayout *>(parent->layout());
				if (!form) continue;

				int                   row = -1;
				QFormLayout::ItemRole role;
				form->getWidgetPosition(ph.label, &row, &role);
				if (row < 0) continue;

				auto *widget = new DelayTableWidget(
					ctx->source,
					selected_ch,
					channels,
					labels,
					parent);

				form->removeRow(row);
				// ラベルなし・フル幅で挿入する。
				form->insertRow(row, widget);
				++replaced_count;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				--ctx->retries_left;
				QTimer::singleShot(kDelayTableInjectRetryMs,
								   [ctx]() { do_delay_table_inject(ctx); });
				return;
			}
			delete ctx;
		}

	} // namespace

	// ============================================================
	// 公開 API
	// ============================================================

	obs_property_t *obs_properties_add_delay_table(
		obs_properties_t            *props,
		const char                  *prop_name,
		int                          selected_ch,
		int                          ch_count,
		const DelayTableChannelInfo *channels,
		const DelayTableLabels      &labels) {
		if (!props || !prop_name || !*prop_name || ch_count <= 0 || !channels)
			return nullptr;

		// 書式: DTABLE|selected_ch|N|hdr_ch|hdr_name|hdr_measured|hdr_base|hdr_adjust|hdr_global|hdr_total|lbl_editor
		//            |ch0_name|ch0_measured|ch0_base|ch0_adjust|ch0_global|ch0_total|ch0_warn|...
		std::string payload = "DTABLE";
		{
			char buf[16];
			std::snprintf(buf, sizeof(buf), "|%d|%d", selected_ch, ch_count);
			payload += buf;
		}
		// 8 label fields
		for (const char *s : {labels.hdr_ch, labels.hdr_name, labels.hdr_measured, labels.hdr_base, labels.hdr_adjust, labels.hdr_global, labels.hdr_total, labels.lbl_editor}) {
			payload += '|';
			payload += escape_field(s ? s : "");
		}
		// per-channel fields
		for (int i = 0; i < ch_count; ++i) {
			const auto &ch = channels[i];
			char        nums[5][32];
			std::snprintf(nums[0], sizeof(nums[0]), "%.6g", static_cast<double>(ch.measured_ms));
			std::snprintf(nums[1], sizeof(nums[1]), "%.6g", static_cast<double>(ch.base_ms));
			std::snprintf(nums[2], sizeof(nums[2]), "%.6g", static_cast<double>(ch.adjust_ms));
			std::snprintf(nums[3], sizeof(nums[3]), "%.6g", static_cast<double>(ch.global_ms));
			std::snprintf(nums[4], sizeof(nums[4]), "%.6g", static_cast<double>(ch.total_ms));
			payload += '|';
			payload += escape_field(ch.name ? ch.name : "");
			payload += '|';
			payload += nums[0];
			payload += '|';
			payload += nums[1];
			payload += '|';
			payload += nums[2];
			payload += '|';
			payload += nums[3];
			payload += '|';
			payload += nums[4];
			payload += '|';
			payload += (ch.warn ? "1" : "0");
		}

		return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
	}

	void schedule_delay_table_inject(obs_source_t *source) {
		if (!source) return;
		auto *ctx = new DelayTableInjectCtx(source);
		obs_queue_task(OBS_TASK_UI, do_delay_table_inject, ctx, false);
	}

} // namespace ods::widgets
