/*
 * url-table-widget.cpp
 *
 * URL 一覧テーブルを QPainter で描画するカスタムウィジェット。
 * ウィジェット幅に応じて QFontMetrics::elidedText で
 * 名前は末尾省略 (Qt::ElideRight)、URL は先頭省略 (Qt::ElideLeft) する。
 */

#include "widgets/url-table-widget.hpp"

#include "ui/table-theme.hpp"
#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QApplication>
#include <QColor>
#include <QDesktopServices>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QMouseEvent>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ods::widgets {

	namespace {

		constexpr char kUrlTableMagic[]        = "URLTBL|";
		constexpr int  kUrlTableInjectRetryMax = 40;
		constexpr int  kUrlTableInjectRetryMs  = 5;

		// レイアウト定数
		constexpr int    kHeaderH   = 24;
		constexpr int    kRowH      = 22;
		constexpr int    kPadX      = 8;
		constexpr int    kPadY      = 3;
		constexpr int    kChColW    = 36;
		constexpr double kNameRatio = 0.30; // 残り幅に対する Name 列の比率

		// パース済みデータ
		struct TableData {
			struct Row {
				int     ch = 0;
				QString name;
				QString url;
			};
			std::vector<Row> rows;
			bool             linkify = true;
			QString          not_configured;
		};

		// ============================================================
		// UrlTableWidget
		// ============================================================

		class UrlTableWidget : public QWidget {
		public:

			explicit UrlTableWidget(const TableData &data, QWidget *parent = nullptr)
				: QWidget(parent), data_(data) {
				setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
				setFixedHeight(calcHeight());
				if (data_.linkify)
					setMouseTracking(true);
			}

		protected:

			void paintEvent(QPaintEvent *) override {
				QPainter p(this);
				p.setRenderHint(QPainter::Antialiasing, false);
				p.setRenderHint(QPainter::TextAntialiasing, true);

				auto theme = ods::ui::make_table_theme_colors(palette());

				QFont f = font();
				f.setPixelSize(11);
				const QFontMetrics fm(f);

				// 列幅の計算
				const int totalW  = width();
				const int restW   = totalW - kChColW;
				const int nameW   = static_cast<int>(restW * kNameRatio);
				const int urlW    = restW - nameW;
				const int colX[3] = {0, kChColW, kChColW + nameW};
				const int colW[3] = {kChColW, nameW, urlW};

				// ヘッダー描画
				{
					p.setPen(Qt::NoPen);
					p.setBrush(theme.header_bg);
					p.drawRect(0, 0, totalW, kHeaderH);

					QFont hf = f;
					hf.setBold(true);
					p.setFont(hf);
					p.setPen(theme.header_text);

					const QString headers[] = {
						QStringLiteral("Ch."),
						QStringLiteral("Name"),
						QStringLiteral("URL"),
					};
					const int hAlign[] = {
						int(Qt::AlignRight | Qt::AlignVCenter),
						int(Qt::AlignLeft | Qt::AlignVCenter),
						int(Qt::AlignLeft | Qt::AlignVCenter),
					};
					for (int c = 0; c < 3; ++c) {
						p.drawText(QRect(colX[c] + kPadX, 0, colW[c] - kPadX * 2, kHeaderH),
								   hAlign[c],
								   headers[c]);
					}

					// ヘッダー下の罫線
					p.setPen(QPen(theme.border, 1));
					p.drawLine(0, kHeaderH - 1, totalW, kHeaderH - 1);
				}

				// 行描画
				p.setFont(f);
				for (int i = 0; i < static_cast<int>(data_.rows.size()); ++i) {
					const auto  &row = data_.rows[i];
					const int    y   = kHeaderH + i * kRowH;
					const QColor bg  = (i % 2 == 0) ? theme.row_bg : theme.alt_row_bg;

					// 行背景
					p.setPen(Qt::NoPen);
					p.setBrush(bg);
					p.drawRect(0, y, totalW, kRowH);

					const QRect chRect(colX[0] + kPadX, y, colW[0] - kPadX * 2, kRowH);
					const QRect nameRect(colX[1] + kPadX, y, colW[1] - kPadX * 2, kRowH);
					const QRect urlRect(colX[2] + kPadX, y, colW[2] - kPadX * 2, kRowH);

					// Ch 番号
					p.setPen(theme.text);
					p.drawText(chRect, int(Qt::AlignRight | Qt::AlignVCenter), QString::number(row.ch));

					// 名前（末尾省略）
					const QString displayName = row.name.isEmpty()
													? QStringLiteral("-")
													: fm.elidedText(row.name, Qt::ElideRight, nameRect.width());
					p.drawText(nameRect, int(Qt::AlignLeft | Qt::AlignVCenter), displayName);

					// URL（先頭省略）
					const QString rawUrl     = row.url.isEmpty() ? data_.not_configured : row.url;
					const QString displayUrl = fm.elidedText(rawUrl, Qt::ElideLeft, urlRect.width());
					if (data_.linkify && !row.url.isEmpty())
						p.setPen(theme.link);
					else
						p.setPen(theme.text);
					p.drawText(urlRect, int(Qt::AlignLeft | Qt::AlignVCenter), displayUrl);
				}

				// 外枠
				p.setPen(QPen(theme.border, 1));
				p.setBrush(Qt::NoBrush);
				p.drawRect(0, 0, totalW - 1, calcHeight() - 1);

				// 列区切り線
				for (int c = 1; c < 3; ++c) {
					const int x = colX[c];
					p.drawLine(x, 0, x, calcHeight() - 1);
				}
			}

			void resizeEvent(QResizeEvent *event) override {
				QWidget::resizeEvent(event);
				update();
			}

			void mousePressEvent(QMouseEvent *event) override {
				if (!data_.linkify) return;
				const int row = hitTestRow(event->pos().y());
				if (row < 0 || row >= static_cast<int>(data_.rows.size())) return;
				const auto &r = data_.rows[row];
				if (r.url.isEmpty()) return;

				// URL 列の範囲内かチェック
				const int urlColX = kChColW + static_cast<int>((width() - kChColW) * kNameRatio);
				if (event->pos().x() >= urlColX)
					QDesktopServices::openUrl(QUrl(r.url));
			}

			void mouseMoveEvent(QMouseEvent *event) override {
				if (!data_.linkify) {
					setCursor(Qt::ArrowCursor);
					return;
				}
				const int row = hitTestRow(event->pos().y());
				if (row < 0 || row >= static_cast<int>(data_.rows.size())) {
					setCursor(Qt::ArrowCursor);
					return;
				}
				const int urlColX = kChColW + static_cast<int>((width() - kChColW) * kNameRatio);
				if (event->pos().x() >= urlColX && !data_.rows[row].url.isEmpty())
					setCursor(Qt::PointingHandCursor);
				else
					setCursor(Qt::ArrowCursor);
			}

		private:

			int calcHeight() const {
				return kHeaderH + static_cast<int>(data_.rows.size()) * kRowH;
			}

			int hitTestRow(int y) const {
				if (y < kHeaderH) return -1;
				return (y - kHeaderH) / kRowH;
			}

			TableData data_;
		};

		// ============================================================
		// inject インフラ
		// ============================================================

		struct UrlTableInjectCtx {
			explicit UrlTableInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {}
			~UrlTableInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kUrlTableInjectRetryMax;
		};

		bool parse_url_table_payload(const QString &text, TableData &out) {
			QStringList fields;
			if (!split_escaped_pipe_fields(text, fields))
				return false;
			if (fields.empty() || fields[0] != QLatin1String("URLTBL"))
				return false;

			// URLTBL|linkify|not_configured|ch_count|ch1|name1|url1|...
			constexpr int kFixedFields = 4; // magic + linkify + not_configured + ch_count
			if (fields.size() < kFixedFields)
				return false;

			out.linkify        = (fields[1] == QLatin1String("1"));
			out.not_configured = fields[2];

			bool ok       = false;
			int  ch_count = fields[3].toInt(&ok);
			if (!ok || ch_count < 0) return false;

			constexpr int kPerRow = 3; // ch, name, url
			if (fields.size() < kFixedFields + ch_count * kPerRow)
				return false;

			out.rows.resize(ch_count);
			int idx = kFixedFields;
			for (int i = 0; i < ch_count; ++i) {
				out.rows[i].ch   = fields[idx++].toInt();
				out.rows[i].name = fields[idx++];
				out.rows[i].url  = fields[idx++];
			}
			return true;
		}

		void do_url_table_inject(void *param) {
			auto ctx = std::unique_ptr<UrlTableInjectCtx>(
				static_cast<UrlTableInjectCtx *>(param));
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
				if (text.startsWith(QLatin1String(kUrlTableMagic)))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found)
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

			int replaced_count = 0;
			for (const auto &ph : found) {
				TableData data;
				if (!parse_url_table_payload(ph.text, data))
					continue;

				QWidget *parent = ph.label->parentWidget();
				if (!parent) continue;
				auto *form = qobject_cast<QFormLayout *>(parent->layout());
				if (!form) continue;

				int                   row = -1;
				QFormLayout::ItemRole role;
				form->getWidgetPosition(ph.label, &row, &role);
				if (row < 0) continue;

				auto *widget = new UrlTableWidget(data, parent);

				form->removeRow(row);
				form->insertRow(row, widget);
				++replaced_count;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kUrlTableInjectRetryMs,
								   [next]() { do_url_table_inject(next); });
				return;
			}
		}

	} // namespace

	// ============================================================
	// 公開 API
	// ============================================================

	obs_property_t *obs_properties_add_url_table(
		obs_properties_t   *props,
		const char         *prop_name,
		const UrlTableInfo &info) {
		if (!props || !prop_name || !*prop_name)
			return nullptr;

		// URLTBL|linkify|not_configured|ch_count|ch1|name1|url1|...
		std::string payload = "URLTBL";
		payload += '|';
		payload += (info.linkify ? "1" : "0");
		payload += '|';
		payload += escape_field(info.not_configured);
		{
			char buf[16];
			std::snprintf(buf, sizeof(buf), "|%d", info.ch_count);
			payload += buf;
		}
		for (int i = 0; i < info.ch_count; ++i) {
			const auto &r = info.rows[i];
			char        ch_str[8];
			std::snprintf(ch_str, sizeof(ch_str), "%d", r.ch_1indexed);
			payload += '|';
			payload += ch_str;
			payload += '|';
			payload += escape_field(r.name);
			payload += '|';
			payload += escape_field(r.url);
		}

		return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
	}

	void schedule_url_table_inject(obs_source_t *source) {
		if (!source) return;
		auto ctx = std::make_unique<UrlTableInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_url_table_inject, ctx.release(), false);
	}

} // namespace ods::widgets
