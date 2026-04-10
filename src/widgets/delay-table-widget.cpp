#include "widgets/delay-table-widget.hpp"

#include "ui/props-refresh.hpp"
#include "ui/table-theme.hpp"
#include "widgets/focus-spin-box.hpp"
#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QColor>
#include <QFontMetrics>
#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyledItemDelegate>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace ods::widgets {

	namespace {

		constexpr char kDelayTableMagic[]        = "DTABLE|";
		constexpr int  kDelayTableInjectRetryMax = 40;
		constexpr int  kDelayTableInjectRetryMs  = 5;

		// 2段ヘッダー行を確実に塗り分けるための専用 delegate。
		class DelayTableItemDelegate : public QStyledItemDelegate {
		public:

			DelayTableItemDelegate(const QColor &header_bg, const QColor &header_text, QObject *parent = nullptr)
				: QStyledItemDelegate(parent), header_bg_(header_bg), header_text_(header_text) {
			}

			void paint(QPainter                   *painter,
					   const QStyleOptionViewItem &option,
					   const QModelIndex          &index) const override {
				if (!painter || !index.isValid()) {
					QStyledItemDelegate::paint(painter, option, index);
					return;
				}
				if (index.row() >= 1) {
					QStyledItemDelegate::paint(painter, option, index);
					return;
				}

				painter->save();
				painter->fillRect(option.rect, header_bg_);
				painter->setPen(header_text_);
				QFont f = option.font;
				f.setBold(true);
				painter->setFont(f);

				int            align      = Qt::AlignCenter;
				const QVariant align_role = index.data(Qt::TextAlignmentRole);
				if (align_role.isValid()) {
					align = align_role.toInt();
				}
				const QString text = index.data(Qt::DisplayRole).toString();
				const QRect   r    = option.rect.adjusted(4, 0, -4, 0);
				painter->drawText(r, align | Qt::TextSingleLine, text);
				painter->restore();
			}

		private:

			QColor header_bg_;
			QColor header_text_;
		};

		// ペイロードをパースしたチャンネル情報。
		struct ParsedChannel {
			QString name;
			double  measured_ms; // -1 if not measured
			int     offset_ms;
			int     raw_delay_ms;
			int     neg_max_ms;
			int     total_ms;
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
							 const QStringList                &labels, // hdr_ch,name,measured,offset,raw,floor,total,lbl_editor
							 QWidget                          *parent = nullptr)
				: QWidget(parent), source_(source ? obs_source_get_ref(source) : nullptr), ch_count_(static_cast<int>(channels.size())), selected_ch_(selected_ch >= 0 && selected_ch < static_cast<int>(channels.size()) ? selected_ch : 0) {
				auto *vlay = new QVBoxLayout(this);
				vlay->setContentsMargins(0, 2, 0, 2);
				vlay->setSpacing(6);
				setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

				constexpr int kNumCols = 7;
				// テーブル（ヘッダー1段 + データ行）
				table_ = new QTableWidget(ch_count_ + 1, kNumCols, this);
				table_->setSelectionBehavior(QAbstractItemView::SelectRows);
				table_->setSelectionMode(QAbstractItemView::SingleSelection);
				table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
				table_->setFrameShape(QFrame::NoFrame);
				table_->setLineWidth(0);
				table_->verticalHeader()->hide();
				table_->setShowGrid(true);
				table_->setAlternatingRowColors(false);
				table_->setFocusPolicy(Qt::NoFocus);
				table_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
				table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
				table_->horizontalHeader()->hide();
				{
					const auto    theme      = ods::ui::make_table_theme_colors(QApplication::palette());
					const QString border     = theme.border.name(QColor::HexRgb);
					const QString table_bg   = theme.row_bg.name(QColor::HexRgb);
					const QString normal_txt = theme.text.name(QColor::HexRgb);
					table_->setStyleSheet(
						QStringLiteral(
							"QTableWidget {"
							" border:1px solid %1;"
							" border-radius:0px;"
							" background-color:%2;"
							" gridline-color:%1;"
							" color:%3;"
							"}"
							"QTableWidget::item {"
							" border:0px;"
							" border-radius:0px;"
							" margin:0px;"
							" padding-top:0px;"
							" padding-bottom:0px;"
							" padding-left:4px;"
							" padding-right:4px;"
							"}"
							"QTableWidget::item:selected {"
							" border:0px;"
							" border-radius:0px;"
							" margin:0px;"
							" padding-top:0px;"
							" padding-bottom:0px;"
							" background-color:palette(highlight);"
							" color:palette(highlighted-text);"
							"}")
							.arg(border, table_bg, normal_txt));
				}

				{
					auto *hdr = table_->horizontalHeader();
					hdr->setSectionResizeMode(0, QHeaderView::ResizeToContents); // Ch
					hdr->setSectionResizeMode(1, QHeaderView::Stretch);          // 名前
					hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents); // 計測
					hdr->setSectionResizeMode(3, QHeaderView::ResizeToContents); // Offset
					hdr->setSectionResizeMode(4, QHeaderView::ResizeToContents); // 計算値
					hdr->setSectionResizeMode(5, QHeaderView::ResizeToContents); // 補正
					hdr->setSectionResizeMode(6, QHeaderView::ResizeToContents); // 合計
				}

				const auto theme = ods::ui::make_table_theme_colors(QApplication::palette());
				table_->setItemDelegate(new DelayTableItemDelegate(theme.header_bg, theme.header_text, table_));

				auto makeHeaderItem = [&theme](const QString &text) {
					auto *item = new QTableWidgetItem(text);
					item->setTextAlignment(Qt::AlignCenter);
					item->setFlags(Qt::ItemIsEnabled);
					QFont f = item->font();
					f.setBold(true);
					item->setFont(f);
					return item;
				};

				// ヘッダー行
				for (int c = 0; c < kNumCols && c < labels.size(); ++c)
					table_->setItem(0, c, makeHeaderItem(labels.value(c)));

				for (int i = 0; i < ch_count_; ++i) {
					const auto  &ch  = channels[i];
					const int    row = i + 1;
					const bool   alt = (i % 2) != 0;
					const QColor row_bg =
						alt ? theme.alt_row_bg : theme.row_bg;

					auto makeItem = [&theme, &row_bg](const QString &text,
													  Qt::Alignment  align = Qt::AlignRight | Qt::AlignVCenter) {
						auto *item = new QTableWidgetItem(text);
						item->setTextAlignment(align);
						item->setBackground(row_bg);
						item->setForeground(theme.text);
						return item;
					};

					table_->setItem(row, 0, makeItem(QString::number(i + 1)));
					table_->setItem(row, 1, makeItem(ch.name.isEmpty() ? QStringLiteral("—") : ch.name, Qt::AlignLeft | Qt::AlignVCenter));
					table_->setItem(row, 2, makeItem(ch.measured_ms < 0.0 ? QStringLiteral("—") : QString::number(ch.measured_ms, 'f', 0)));
					table_->setItem(row, 3, makeItem(QString::number(ch.offset_ms)));

					auto *raw_item = makeItem(QString::number(ch.raw_delay_ms));
					if (ch.warn) {
						raw_item->setForeground(QColor(Qt::red));
						QFont f = raw_item->font();
						f.setBold(true);
						raw_item->setFont(f);
					}
					table_->setItem(row, 4, raw_item);
					table_->setItem(row, 5, makeItem(ch.neg_max_ms > 0 ? QStringLiteral("+%1").arg(ch.neg_max_ms) : QStringLiteral("0")));
					table_->setItem(row, 6, makeItem(QString::number(ch.total_ms)));
				}

				// 行の上下パディングを詰めて固定する。
				table_->verticalHeader()->setDefaultSectionSize(22);
				table_->setRowHeight(0, 20);
				{
					int h = table_->horizontalHeader()->height() + 4;
					for (int i = 0; i < ch_count_ + 1; ++i)
						h += table_->rowHeight(i);
					table_->setFixedHeight(h);
				}

				connect(table_, &QTableWidget::itemSelectionChanged, this, &DelayTableWidget::onSelectionChanged);

				vlay->addWidget(table_);

				// 選択チャンネルの「アジャスト」エディタ
				// 順序は StepperRow に合わせる: [ラベル] [Reset] [Spin] [デルタボタン群]
				auto *editor_row = new QWidget(this);
				editor_row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
				auto *hlay = new QHBoxLayout(editor_row);
				hlay->setContentsMargins(0, 0, 0, 0);
				hlay->setSpacing(4);
				hlay->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

				editor_label_base_ = labels.value(7).trimmed();
				if (editor_label_base_.isEmpty())
					editor_label_base_ = QStringLiteral("Offset");
				editor_label_ = new QLabel(this);
				editor_label_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
				hlay->addWidget(editor_label_);

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
				spin_->setRange(-3000.0, 3000.0); // SUB_ADJUST_MIN_MS .. SUB_ADJUST_MAX_MS
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

				vlay->addWidget(editor_row, 0, Qt::AlignLeft | Qt::AlignTop);

				connect(spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &DelayTableWidget::onValueChanged);

				// 初期行を選択してスピンを同期する。
				table_->blockSignals(true);
				if (ch_count_ > 0)
					table_->selectRow(selected_ch_ + 1);
				table_->blockSignals(false);
				updateEditorLabel();
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
					if (row > 0 && row <= ch_count_)
						selected_ch_ = row - 1;
				}
				updateEditorLabel();
				saveSelectedChannel();
				loadCurrentChannelValue();
			}

			// 選択中チャンネルに追従してエディタ左ラベルを更新する。
			void updateEditorLabel() {
				if (!editor_label_) return;
				if (selected_ch_ >= 0 && selected_ch_ < ch_count_) {
					editor_label_->setText(
						QStringLiteral("Ch.%1 %2")
							.arg(selected_ch_ + 1)
							.arg(editor_label_base_));
					return;
				}
				editor_label_->setText(editor_label_base_);
			}

			// OBS設定から選択中チャンネルの offset を読んでスピンに反映する。
			void loadCurrentChannelValue() {
				if (!source_ || selected_ch_ < 0 || selected_ch_ >= ch_count_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s) return;
				char key[32];
				std::snprintf(key, sizeof(key), "sub%d_adjust_ms", selected_ch_);
				const int v = static_cast<int>(obs_data_get_int(s, key));
				obs_data_release(s);
				spin_->blockSignals(true);
				spin_->setValue(static_cast<double>(v));
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

			// スピン値が変わったとき: OBS 設定の sub{N}_adjust_ms を更新し、
			// テーブル再描画のためにプロパティ再構築を要求する。
			void onValueChanged(double val) {
				if (!source_ || selected_ch_ < 0 || selected_ch_ >= ch_count_)
					return;
				obs_data_t *s = obs_source_get_settings(source_);
				if (!s) return;
				char key[32];
				std::snprintf(key, sizeof(key), "sub%d_adjust_ms", selected_ch_);
				obs_data_set_int(s, key, static_cast<int>(std::lround(val)));
				obs_source_update(source_, s);
				obs_data_release(s);
				ods::ui::props_refresh_request(source_, true, false, 0, "delay_table_offset");
			}

			obs_source_t *source_      = nullptr;
			int           ch_count_    = 0;
			int           selected_ch_ = 0;
			QString       editor_label_base_;
			QTableWidget *table_        = nullptr;
			QLabel       *editor_label_ = nullptr;
			FocusSpinBox *spin_         = nullptr;
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
		// 書式: DTABLE|selected_ch|N|hdr_ch|hdr_name|hdr_measured|hdr_offset|hdr_raw|hdr_floor|hdr_total|lbl_editor
		//            |ch0_name|ch0_measured|ch0_offset|ch0_raw|ch0_neg_max|ch0_total|ch0_warn|...
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

			constexpr int kLabelFields = 8;
			constexpr int kChFields    = 7;
			constexpr int kHeaderSize  = 3 + kLabelFields;
			if (static_cast<int>(fields.size()) < kHeaderSize + n * kChFields)
				return false;

			labels = fields.mid(3, kLabelFields);

			channels.clear();
			channels.reserve(static_cast<size_t>(n));
			int idx = kHeaderSize;
			for (int i = 0; i < n; ++i) {
				ParsedChannel ch;
				ch.name         = fields[idx++];
				ch.measured_ms  = fields[idx++].toDouble();
				ch.offset_ms    = fields[idx++].toInt();
				ch.raw_delay_ms = fields[idx++].toInt();
				ch.neg_max_ms   = fields[idx++].toInt();
				ch.total_ms     = fields[idx++].toInt();
				ch.warn         = (fields[idx++] == QLatin1String("1"));
				channels.push_back(ch);
			}
			return true;
		}

		/// DelayTable プレースホルダーを実テーブルウィジェットへ差し替える。
		void do_delay_table_inject(void *param) {
			auto ctx = std::unique_ptr<DelayTableInjectCtx>(static_cast<DelayTableInjectCtx *>(param));
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
				auto *next = ctx.release();
				QTimer::singleShot(kDelayTableInjectRetryMs,
								   [next]() { do_delay_table_inject(next); });
				return;
			}
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

		// 書式: DTABLE|selected_ch|N|hdr_ch|hdr_name|hdr_measured|hdr_offset|hdr_raw|hdr_floor|hdr_total|lbl_editor
		//            |ch0_name|ch0_measured|ch0_offset|ch0_raw|ch0_neg_max|ch0_total|ch0_warn|...
		std::string payload = "DTABLE";
		{
			char buf[16];
			std::snprintf(buf, sizeof(buf), "|%d|%d", selected_ch, ch_count);
			payload += buf;
		}
		// 8 label fields
		for (const char *s : {labels.hdr_ch, labels.hdr_name, labels.hdr_measured, labels.hdr_adjust, labels.hdr_raw, labels.hdr_floor, labels.hdr_total, labels.lbl_editor}) {
			payload += '|';
			payload += escape_field(s ? s : "");
		}
		// per-channel fields (7 per channel)
		for (int i = 0; i < ch_count; ++i) {
			const auto &ch = channels[i];
			char        nums[4][32];
			std::snprintf(nums[0], sizeof(nums[0]), "%.6g", static_cast<double>(ch.measured_ms));
			std::snprintf(nums[1], sizeof(nums[1]), "%d", ch.offset_ms);
			std::snprintf(nums[2], sizeof(nums[2]), "%d", ch.raw_delay_ms);
			std::snprintf(nums[3], sizeof(nums[3]), "%d", ch.neg_max_ms);
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
			{
				char total_buf[16];
				std::snprintf(total_buf, sizeof(total_buf), "%d", ch.total_ms);
				payload += total_buf;
			}
			payload += '|';
			payload += (ch.warn ? "1" : "0");
		}

		return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
	}

	void schedule_delay_table_inject(obs_source_t *source) {
		if (!source) return;
		auto ctx = std::make_unique<DelayTableInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_delay_table_inject, ctx.release(), false);
	}

} // namespace ods::widgets
