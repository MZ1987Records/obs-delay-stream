#include "widgets/flow-table-widget.hpp"

#include "ui/table-theme.hpp"
#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QApplication>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QSizePolicy>
#include <QStyledItemDelegate>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ods::widgets {

	namespace {

		constexpr char kFlowTableMagic[]        = "FLOWTBL|";
		constexpr int  kFlowTableInjectRetryMax = 40;
		constexpr int  kFlowTableInjectRetryMs  = 5;

		// ステータス列に色付き丸（●）を描画するカスタム delegate。
		class FlowTableItemDelegate : public QStyledItemDelegate {
		public:

			FlowTableItemDelegate(const QColor &header_bg, const QColor &header_text, QObject *parent = nullptr)
				: QStyledItemDelegate(parent), header_bg_(header_bg), header_text_(header_text) {
			}

			void paint(QPainter                   *painter,
					   const QStyleOptionViewItem &option,
					   const QModelIndex          &index) const override {
				if (!painter || !index.isValid()) {
					QStyledItemDelegate::paint(painter, option, index);
					return;
				}

				// ヘッダー行（row 0）の描画
				if (index.row() == 0) {
					painter->save();
					painter->fillRect(option.rect, header_bg_);
					painter->setPen(header_text_);
					QFont f = option.font;
					f.setBold(true);
					painter->setFont(f);

					int            align      = Qt::AlignCenter;
					const QVariant align_role = index.data(Qt::TextAlignmentRole);
					if (align_role.isValid())
						align = align_role.toInt();
					const QString text = index.data(Qt::DisplayRole).toString();
					const QRect   r    = option.rect.adjusted(4, 0, -4, 0);
					painter->drawText(r, align | Qt::TextSingleLine, text);
					painter->restore();
					return;
				}

				// データ行: UserRole に色が格納されていれば丸を描画
				const QVariant dot_var = index.data(Qt::UserRole);
				if (dot_var.isValid() && dot_var.canConvert<QColor>()) {
					// 背景とフォーカス描画
					QStyleOptionViewItem opt = option;
					initStyleOption(&opt, index);
					QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
					style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

					const QColor  dot_color = dot_var.value<QColor>();
					const QString text      = index.data(Qt::DisplayRole).toString();
					const QRect   r         = option.rect.adjusted(6, 0, -4, 0);

					painter->save();
					// 丸を描画
					const int dot_y = r.top() + (r.height() - kDotSize) / 2;
					painter->setRenderHint(QPainter::Antialiasing, true);
					painter->setBrush(dot_color);
					painter->setPen(Qt::NoPen);
					painter->drawEllipse(r.left(), dot_y, kDotSize, kDotSize);

					// テキストを描画
					painter->setRenderHint(QPainter::Antialiasing, false);
					painter->setPen(opt.palette.color(
						(opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text));
					painter->setFont(opt.font);
					const QRect text_rect(r.left() + kDotSize + kDotGap, r.top(), r.width() - kDotSize - kDotGap, r.height());
					painter->drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text);
					painter->restore();
					return;
				}

				QStyledItemDelegate::paint(painter, option, index);
			}

			QSize sizeHint(const QStyleOptionViewItem &option,
						   const QModelIndex          &index) const override {
				QSize s = QStyledItemDelegate::sizeHint(option, index);
				if (index.row() > 0) {
					const QVariant dot_var = index.data(Qt::UserRole);
					if (dot_var.isValid() && dot_var.canConvert<QColor>())
						s.setWidth(s.width() + kDotSize + kDotGap + 4);
				}
				return s;
			}

		private:

			static constexpr int kDotSize = 8;
			static constexpr int kDotGap  = 5;

			QColor header_bg_;
			QColor header_text_;
		};

		// ペイロードをパースしたチャンネル情報。
		struct ParsedFlowChannel {
			QString name;
			bool    connected;
			int     measured_ms; // -1: not measured, -2: failed, >=0: result
		};

		// ============================================================
		// FlowTableWidget
		// ============================================================
		class FlowTableWidget : public QWidget {
		public:

			FlowTableWidget(int                                   ch_count,
							const std::vector<ParsedFlowChannel> &channels,
							const QStringList                    &labels,
							QWidget                              *parent = nullptr)
				: QWidget(parent) {
				auto *vlay = new QVBoxLayout(this);
				vlay->setContentsMargins(0, 2, 0, 2);
				vlay->setSpacing(0);
				setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

				constexpr int kNumCols = 4;
				auto         *table    = new QTableWidget(ch_count + 1, kNumCols, this);
				table->setSelectionMode(QAbstractItemView::NoSelection);
				table->setEditTriggers(QAbstractItemView::NoEditTriggers);
				table->setFrameShape(QFrame::NoFrame);
				table->setLineWidth(0);
				table->verticalHeader()->hide();
				table->setShowGrid(true);
				table->setAlternatingRowColors(false);
				table->setFocusPolicy(Qt::NoFocus);
				table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
				table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
				table->horizontalHeader()->hide();

				const auto theme = ods::ui::make_table_theme_colors(QApplication::palette());
				{
					const QString border     = theme.border.name(QColor::HexRgb);
					const QString table_bg   = theme.row_bg.name(QColor::HexRgb);
					const QString normal_txt = theme.text.name(QColor::HexRgb);
					table->setStyleSheet(
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
							"}")
							.arg(border, table_bg, normal_txt));
				}

				{
					auto *hdr = table->horizontalHeader();
					hdr->setSectionResizeMode(0, QHeaderView::ResizeToContents); // Ch
					hdr->setSectionResizeMode(1, QHeaderView::Stretch);          // ���前
					hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents); // 状態
					hdr->setSectionResizeMode(3, QHeaderView::ResizeToContents); // 計測結果
				}

				table->setItemDelegate(new FlowTableItemDelegate(theme.header_bg, theme.header_text, table));

				auto makeHeaderItem = [&](const QString &text) {
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
					table->setItem(0, c, makeHeaderItem(labels.value(c)));

				// ステータスラベルは labels[4]=connected, labels[5]=disconnected, labels[6]=failed
				const QString str_connected    = labels.value(4);
				const QString str_disconnected = labels.value(5);
				const QString str_failed       = labels.value(6);

				const QColor color_connected(0x22, 0xc5, 0x5e);    // 緑
				const QColor color_disconnected(0x9c, 0xa3, 0xaf); // グレー

				for (int i = 0; i < ch_count; ++i) {
					const auto  &ch     = channels[i];
					const int    row    = i + 1;
					const bool   alt    = (i % 2) != 0;
					const QColor row_bg = alt ? theme.alt_row_bg : theme.row_bg;

					auto makeItem = [&](const QString &text,
										Qt::Alignment  align = Qt::AlignRight | Qt::AlignVCenter) {
						auto *item = new QTableWidgetItem(text);
						item->setTextAlignment(align);
						item->setBackground(row_bg);
						item->setForeground(theme.text);
						return item;
					};

					// Ch
					table->setItem(row, 0, makeItem(QString::number(i + 1)));

					// 名前
					table->setItem(row, 1, makeItem(ch.name.isEmpty() ? QStringLiteral("\u2014") : ch.name, Qt::AlignLeft | Qt::AlignVCenter));

					// 状態（色付き丸 + テキスト）
					{
						const QString &status_text  = ch.connected ? str_connected : str_disconnected;
						const QColor  &status_color = ch.connected ? color_connected : color_disconnected;
						auto          *item         = makeItem(status_text, Qt::AlignLeft | Qt::AlignVCenter);
						item->setData(Qt::UserRole, status_color);
						table->setItem(row, 2, item);
					}

					// 計測結果
					{
						QString result_text;
						if (ch.measured_ms >= 0) {
							result_text = QStringLiteral("%1 ms").arg(ch.measured_ms);
						} else if (ch.measured_ms == -2) {
							result_text = str_failed;
						} else {
							result_text = QStringLiteral("\u2014");
						}
						table->setItem(row, 3, makeItem(result_text));
					}
				}

				// 行の上下パディングを詰めて固定する。
				table->verticalHeader()->setDefaultSectionSize(22);
				table->setRowHeight(0, 20);
				{
					int h = 2; // border top + bottom
					for (int i = 0; i < ch_count + 1; ++i)
						h += table->rowHeight(i);
					table->setFixedHeight(h);
				}

				vlay->addWidget(table);
			}
		};

		// ============================================================
		// inject インフラ
		// ============================================================
		struct FlowTableInjectCtx {
			explicit FlowTableInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {
			}
			~FlowTableInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kFlowTableInjectRetryMax;
		};

		// ペイロード文字列をパースする。
		// 書式: FLOWTBL|N|hdr_ch|hdr_name|hdr_status|hdr_result|str_connected|str_disconnected|str_failed
		//            |ch0_name|ch0_connected|ch0_measured_ms|...
		bool parse_flow_table_payload(const QString                  &text,
									  QStringList                    &labels,
									  std::vector<ParsedFlowChannel> &channels) {
			QStringList fields;
			if (!split_escaped_pipe_fields(text, fields))
				return false;
			if (fields.empty() || fields[0] != QLatin1String("FLOWTBL"))
				return false;
			if (fields.size() < 2)
				return false;

			bool      ok = false;
			const int n  = fields[1].toInt(&ok);
			if (!ok || n <= 0) return false;

			constexpr int kLabelFields = 7; // hdr_ch, hdr_name, hdr_status, hdr_result, str_connected, str_disconnected, str_failed
			constexpr int kChFields    = 3; // name, connected, measured_ms
			constexpr int kHeaderSize  = 2 + kLabelFields;
			if (static_cast<int>(fields.size()) < kHeaderSize + n * kChFields)
				return false;

			labels = fields.mid(2, kLabelFields);

			channels.clear();
			channels.reserve(static_cast<size_t>(n));
			int idx = kHeaderSize;
			for (int i = 0; i < n; ++i) {
				ParsedFlowChannel ch;
				ch.name        = fields[idx++];
				ch.connected   = (fields[idx++] == QLatin1String("1"));
				ch.measured_ms = fields[idx++].toInt();
				channels.push_back(ch);
			}
			return true;
		}

		/// FlowTable プレースホルダーを実テーブルウィジェットへ差し替える。
		void do_flow_table_inject(void *param) {
			auto ctx = std::unique_ptr<FlowTableInjectCtx>(static_cast<FlowTableInjectCtx *>(param));
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
				if (text.startsWith(QLatin1String(kFlowTableMagic)))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found)
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

			int replaced_count = 0;
			for (const auto &ph : found) {
				QStringList                    labels;
				std::vector<ParsedFlowChannel> channels;
				if (!parse_flow_table_payload(ph.text, labels, channels))
					continue;

				QWidget *parent = ph.label->parentWidget();
				if (!parent) continue;
				auto *form = qobject_cast<QFormLayout *>(parent->layout());
				if (!form) continue;

				int                   row = -1;
				QFormLayout::ItemRole role;
				form->getWidgetPosition(ph.label, &row, &role);
				if (row < 0) continue;

				auto *widget = new FlowTableWidget(
					static_cast<int>(channels.size()),
					channels,
					labels,
					parent);

				form->removeRow(row);
				form->insertRow(row, widget);
				++replaced_count;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kFlowTableInjectRetryMs,
								   [next]() { do_flow_table_inject(next); });
				return;
			}
		}

	} // namespace

	// ============================================================
	// 公開 API
	// ============================================================

	obs_property_t *obs_properties_add_flow_table(
		obs_properties_t           *props,
		const char                 *prop_name,
		int                         ch_count,
		const FlowTableChannelInfo *channels,
		const FlowTableLabels      &labels) {
		if (!props || !prop_name || !*prop_name || ch_count <= 0 || !channels)
			return nullptr;

		// 書式: FLOWTBL|N|hdr_ch|hdr_name|hdr_status|hdr_result|str_connected|str_disconnected|str_failed
		//            |ch0_name|ch0_connected|ch0_measured_ms|...
		std::string payload = "FLOWTBL";
		{
			char buf[16];
			std::snprintf(buf, sizeof(buf), "|%d", ch_count);
			payload += buf;
		}
		// 7 label fields
		for (const char *s : {labels.hdr_ch, labels.hdr_name, labels.hdr_status, labels.hdr_result, labels.status_connected, labels.status_disconnected, labels.result_failed}) {
			payload += '|';
			payload += escape_field(s ? s : "");
		}
		// per-channel fields (3 per channel)
		for (int i = 0; i < ch_count; ++i) {
			const auto &ch = channels[i];
			payload += '|';
			payload += escape_field(ch.name ? ch.name : "");
			payload += '|';
			payload += ch.connected ? "1" : "0";
			payload += '|';
			char ms_buf[16];
			std::snprintf(ms_buf, sizeof(ms_buf), "%d", ch.measured_ms);
			payload += ms_buf;
		}

		return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
	}

	void schedule_flow_table_inject(obs_source_t *source) {
		if (!source) return;
		auto ctx = std::make_unique<FlowTableInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_flow_table_inject, ctx.release(), false);
	}

} // namespace ods::widgets
