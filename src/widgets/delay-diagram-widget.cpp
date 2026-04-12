/*
 * delay-diagram-widget.cpp
 *
 * HTML プロトタイプ (docs/delay-diagram.html) の SVG タイミング図を
 * QPainter カスタムウィジェットに移植したもの。
 *
 * レーン構成:
 *   Ch.N : [ディレイ total_ms] [WS C[i]] ([環境 offset[i]]) [再生バッファ] [アバター遅延 A]
 *   ※ offset < 0 の場合は WS+offset を合算し環境セグメントを省略
 *   配信 : [ディレイ master]   [OBS配信遅延 R]
 *
 * 全レーンの右端を揃えて「同期点」を視覚化する。
 */

#include "widgets/delay-diagram-widget.hpp"

#include "widgets/widget-inject-utils.hpp"
#include "widgets/widget-payload-utils.hpp"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ods::widgets {

	namespace {

		constexpr char kDiagramMagic[]        = "DDIAGRAM|";
		constexpr int  kDiagramInjectRetryMax = 40;
		constexpr int  kDiagramInjectRetryMs  = 5;

		// セグメント配色（HTML プロトタイプと同一）
		inline QColor colorDelay() {
			return QColor(239, 68, 68);
		} // #ef4444
		inline QColor colorWs() {
			return QColor(37, 99, 235);
		} // #2563eb
		inline QColor colorEnv() {
			return QColor(139, 92, 246);
		} // #8b5cf6
		inline QColor colorBuf() {
			return QColor(75, 85, 99);
		} // #4b5563
		inline QColor colorAvatar() {
			return QColor(245, 158, 11);
		} // #f59e0b
		inline QColor colorBroadcast() {
			return QColor(20, 184, 166);
		} // #14b8a6

		// 描画に必要なパース済みデータ。
		struct DiagramData {
			int R            = 0;
			int A            = 0;
			int buf          = 0;
			int ch_count     = 0;
			int master_delay = 0;

			struct Ch {
				float measured_ms = -1.0f;
				int   total_ms    = 0;
				int   offset_ms   = 0;
			};
			std::vector<Ch> channels;

			// 凡例ラベル（9 個）
			QString lbl_delay;
			QString lbl_delay_desc;
			QString lbl_ws;
			QString lbl_env;
			QString lbl_buf;
			QString lbl_avatar;
			QString lbl_broadcast;
			QString lbl_lane_broadcast;
			QString lbl_no_data;
			QString lbl_no_data_rtsp;
			QString lbl_no_data_ws;
		};

		// レーン内のセグメント 1 つ。
		struct Segment {
			int    ms;
			QColor color;
		};

		// レーン 1 本。
		struct Lane {
			QString              label;
			std::vector<Segment> segments;

			int total_ms() const {
				int t = 0;
				for (const auto &s : segments)
					t += std::max(0, s.ms);
				return t;
			}
		};

		// ============================================================
		// レーンの構築
		// ============================================================

		Lane build_channel_lane(int ch_index, int total_ms, float C_ms, int offset, int A, int buf) {
			Lane lane;
			lane.label      = QStringLiteral("Ch.%1").arg(ch_index + 1);
			const int C_int = static_cast<int>(std::round(C_ms));
			if (offset >= 0) {
				lane.segments = {
					{total_ms, colorDelay()},
					{C_int, colorWs()},
					{offset, colorEnv()},
					{buf, colorBuf()},
					{A, colorAvatar()},
				};
			} else {
				lane.segments = {
					{total_ms, colorDelay()},
					{C_int + offset, colorWs()},
					{buf, colorBuf()},
					{A, colorAvatar()},
				};
			}
			return lane;
		}

		Lane build_broadcast_lane(const QString &label, int master_delay, int R) {
			Lane lane;
			lane.label    = label;
			lane.segments = {
				{master_delay, colorDelay()},
				{R, colorBroadcast()},
			};
			return lane;
		}

		// ============================================================
		// DelayDiagramWidget
		// ============================================================

		constexpr int kLaneH         = 28;
		constexpr int kLaneGap       = 10;
		constexpr int kRulerH        = 20;
		constexpr int kRulerMarginB  = 8;
		constexpr int kLegendH       = 22;
		constexpr int kLegendMarginT = 10;
		constexpr int kMarginTop     = 4;
		constexpr int kMarginBottom  = 4;
		constexpr int kMarginL       = 48;
		constexpr int kMarginR       = 12;
		constexpr int kMinSegW       = 3;

		class DelayDiagramWidget : public QWidget {
		public:

			explicit DelayDiagramWidget(const DiagramData &data, QWidget *parent = nullptr)
				: QWidget(parent), data_(data) {
				setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
				setFixedHeight(calcHeight());
			}

		protected:

			void paintEvent(QPaintEvent * /*event*/) override {
				QPainter p(this);
				p.setRenderHint(QPainter::Antialiasing, false);
				p.setRenderHint(QPainter::TextAntialiasing, true);

				// 計測データがない場合
				if (!hasData()) {
					drawNoData(p);
					return;
				}

				// レーンを構築
				std::vector<Lane> lanes;
				for (int i = 0; i < data_.ch_count; ++i) {
					if (i >= static_cast<int>(data_.channels.size())) break;
					const auto &ch = data_.channels[i];
					if (ch.measured_ms < 0.0f) continue;
					lanes.push_back(build_channel_lane(
						i,
						ch.total_ms,
						ch.measured_ms,
						ch.offset_ms,
						data_.A,
						data_.buf));
				}
				lanes.push_back(build_broadcast_lane(
					data_.lbl_lane_broadcast,
					data_.master_delay,
					data_.R));

				// スケーリング
				const int usableW = width() - kMarginL - kMarginR;
				int       maxMs   = 0;
				for (const auto &lane : lanes)
					maxMs = std::max(maxMs, lane.total_ms());
				if (maxMs <= 0) {
					drawNoData(p);
					return;
				}
				const double scale = static_cast<double>(usableW) / maxMs;

				// ルーラー
				const int rulerY = kMarginTop;
				drawRuler(p, rulerY, maxMs, scale);

				// レーン描画
				const int lanesTop = rulerY + kRulerH + kRulerMarginB;
				for (int li = 0; li < static_cast<int>(lanes.size()); ++li) {
					const int y = lanesTop + li * (kLaneH + kLaneGap);
					drawLane(p, lanes[li], y, maxMs, scale);
				}

				// 凡例
				const int legendY = lanesTop + static_cast<int>(lanes.size()) * (kLaneH + kLaneGap) + kLegendMarginT;
				drawLegend(p, legendY);
			}

		private:

			bool hasData() const {
				if (data_.R <= 0) return false;
				for (int i = 0; i < data_.ch_count && i < static_cast<int>(data_.channels.size()); ++i) {
					if (data_.channels[i].measured_ms >= 0.0f) return true;
				}
				return false;
			}

			int countVisibleLanes() const {
				int n = 0;
				for (int i = 0; i < data_.ch_count && i < static_cast<int>(data_.channels.size()); ++i) {
					if (data_.channels[i].measured_ms >= 0.0f) ++n;
				}
				return n + 1; // +1 for broadcast lane
			}

			int noDataLineCount() const {
				int n = 0;
				if (data_.R <= 0) ++n;
				bool has_ws = false;
				for (int i = 0; i < data_.ch_count && i < static_cast<int>(data_.channels.size()); ++i) {
					if (data_.channels[i].measured_ms >= 0.0f) {
						has_ws = true;
						break;
					}
				}
				if (!has_ws) ++n;
				return std::max(n, 1);
			}

			int calcHeight() const {
				if (!hasData()) return 20 + noDataLineCount() * 20;
				const int lanes = countVisibleLanes();
				// 凡例2行: 1行目=レイテンシ系、2行目=自動調整ディレイ説明
				return kMarginTop + kRulerH + kRulerMarginB + lanes * (kLaneH + kLaneGap) + kLegendMarginT + kLegendH + kLegendH + kMarginBottom;
			}

			void drawNoData(QPainter &p) const {
				QStringList lines;
				bool has_ws = false;
				for (int i = 0; i < data_.ch_count && i < static_cast<int>(data_.channels.size()); ++i) {
					if (data_.channels[i].measured_ms >= 0.0f) {
						has_ws = true;
						break;
					}
				}
				if (!has_ws && !data_.lbl_no_data_ws.isEmpty())
					lines << data_.lbl_no_data_ws;
				if (data_.R <= 0 && !data_.lbl_no_data_rtsp.isEmpty())
					lines << data_.lbl_no_data_rtsp;
				if (lines.isEmpty())
					lines << data_.lbl_no_data;

				p.setPen(warningTextColor(palette()));
				QFont f = font();
				f.setPointSize(9);
				p.setFont(f);

				const QFontMetrics fm(f);
				const int          lineH  = fm.height() + 4;
				const int          totalH = lines.size() * lineH;
				int                y      = (height() - totalH) / 2;
				for (const auto &line : lines) {
					p.drawText(QRect(0, y, width(), lineH), int(Qt::AlignCenter), line);
					y += lineH;
				}
			}

			// ルーラー描画
			void drawRuler(QPainter &p, int top, int maxMs, double scale) const {
				const int     baseY = top + kRulerH;
				constexpr int tickH = 5;

				const QColor lineColor = palette().color(QPalette::Mid);
				const QColor textColor = palette().color(QPalette::Disabled, QPalette::Text);

				// ベースライン
				p.setPen(QPen(lineColor, 1));
				p.drawLine(kMarginL, baseY, kMarginL + static_cast<int>(maxMs * scale), baseY);

				// 目盛り間隔を自動決定
				const double rawStep = maxMs / 8.0;
				const double mag     = std::pow(10.0, std::floor(std::log10(rawStep)));
				double       nice    = mag;
				for (double n : {1.0, 2.0, 5.0, 10.0}) {
					if (n * mag >= rawStep) {
						nice = n * mag;
						break;
					}
				}
				const int step = std::max(1, static_cast<int>(nice));

				QFont f = font();
				f.setPixelSize(9);
				p.setFont(f);

				for (int ms = 0; ms <= maxMs; ms += step) {
					const int x = kMarginL + static_cast<int>(ms * scale);
					p.setPen(QPen(lineColor, 1));
					p.drawLine(x, baseY - tickH, x, baseY);

					p.setPen(textColor);
					const QString label = (ms == 0)
											  ? QStringLiteral("0 ms")
											  : QString::number(ms);
					p.drawText(QRect(x - 30, top, 60, kRulerH - tickH - 1),
							   int(Qt::AlignHCenter | Qt::AlignBottom),
							   label);
				}
			}

			// レーン 1 本の描画
			void drawLane(QPainter &p, const Lane &lane, int y, int maxMs, double scale) const {
				// レーンラベル
				{
					const QColor textColor = palette().color(QPalette::Text);
					p.setPen(textColor);
					QFont f = font();
					f.setPixelSize(11);
					f.setBold(true);
					p.setFont(f);
					p.drawText(QRect(0, y, kMarginL - 6, kLaneH),
							   int(Qt::AlignRight | Qt::AlignVCenter),
							   lane.label);
				}

				// 右端揃えのオフセット
				const int totalMs    = lane.total_ms();
				const int laneOffset = static_cast<int>((maxMs - totalMs) * scale);

				// セグメント描画
				int   x       = kMarginL + laneOffset;
				QFont segFont = font();
				segFont.setPixelSize(10);

				for (const auto &seg : lane.segments) {
					if (seg.ms <= 0) continue;
					const int w = std::max(kMinSegW, static_cast<int>(seg.ms * scale));

					// 矩形
					p.setPen(Qt::NoPen);
					p.setBrush(seg.color);
					p.drawRect(x, y, w, kLaneH);

					// セグメント枠線（微妙な区切り）
					p.setPen(QPen(QColor(255, 255, 255, 20), 0.5));
					p.setBrush(Qt::NoBrush);
					p.drawRect(x, y, w, kLaneH);

					// 数値ラベル
					if (w > 22) {
						p.setPen(QColor(255, 255, 255));
						p.setFont(segFont);
						p.drawText(QRect(x, y, w, kLaneH),
								   int(Qt::AlignCenter),
								   QString::number(seg.ms));
					}

					x += w;
				}
			}

			// 凡例描画
			void drawLegend(QPainter &p, int y) const {
				struct LegendItem {
					QColor  color;
					QString label;
				};
				// 1行目: レイテンシ系の凡例（ディレイ以外）
				const std::array<LegendItem, 5> row1Items = {{
					{colorWs(), data_.lbl_ws},
					{colorEnv(), data_.lbl_env},
					{colorBuf(), data_.lbl_buf},
					{colorAvatar(), data_.lbl_avatar},
					{colorBroadcast(), data_.lbl_broadcast},
				}};

				// 凡例の上に区切り線
				const QColor lineColor = palette().color(QPalette::Mid);
				p.setPen(QPen(lineColor, 1));
				p.drawLine(kMarginL, y - kLegendMarginT / 2, width() - kMarginR, y - kLegendMarginT / 2);

				QFont f = font();
				f.setPixelSize(10);
				p.setFont(f);
				const QFontMetrics fm(f);

				constexpr int swatchW = 12;
				constexpr int swatchH = 12;
				constexpr int gap     = 10;
				constexpr int textGap = 4;

				const QColor textColor = palette().color(QPalette::Disabled, QPalette::Text);

				// --- 1行目 ---
				int x = kMarginL;
				for (const auto &item : row1Items) {
					const int sy = y + (kLegendH - swatchH) / 2;
					p.setPen(Qt::NoPen);
					p.setBrush(item.color);
					p.drawRect(x, sy, swatchW, swatchH);

					p.setPen(textColor);
					const int textX = x + swatchW + textGap;
					const int textW = fm.horizontalAdvance(item.label);
					p.drawText(QRect(textX, y, textW + 2, kLegendH),
							   int(Qt::AlignLeft | Qt::AlignVCenter),
							   item.label);

					x = textX + textW + gap;
				}

				// --- 2行目: 自動調整ディレイ説明 ---
				const int y2 = y + kLegendH;
				x            = kMarginL;

				// カラースウォッチ
				const int sy2 = y2 + (kLegendH - swatchH) / 2;
				p.setPen(Qt::NoPen);
				p.setBrush(colorDelay());
				p.drawRect(x, sy2, swatchW, swatchH);

				// ラベル（太字）
				QFont boldFont = f;
				boldFont.setBold(true);
				p.setFont(boldFont);
				const QFontMetrics fmBold(boldFont);

				p.setPen(textColor);
				const int labelX = x + swatchW + textGap;
				const int labelW = fmBold.horizontalAdvance(data_.lbl_delay);
				p.drawText(QRect(labelX, y2, labelW + 2, kLegendH),
						   int(Qt::AlignLeft | Qt::AlignVCenter),
						   data_.lbl_delay);

				// 説明テキスト（通常ウェイト）
				p.setFont(f);
				const int descX = labelX + labelW + 4;
				const int descW = fm.horizontalAdvance(data_.lbl_delay_desc);
				p.drawText(QRect(descX, y2, descW + 2, kLegendH),
						   int(Qt::AlignLeft | Qt::AlignVCenter),
						   data_.lbl_delay_desc);
			}

			DiagramData data_;
		};

		// ============================================================
		// inject インフラ
		// ============================================================

		struct DiagramInjectCtx {
			explicit DiagramInjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {}
			~DiagramInjectCtx() {
				if (source) {
					obs_source_release(source);
					source = nullptr;
				}
			}
			obs_source_t *source       = nullptr;
			int           retries_left = kDiagramInjectRetryMax;
		};

		// ペイロード文字列をパースする。
		// 書式: DDIAGRAM|R|A|buf|chCount|master_delay
		//       |lbl_delay|lbl_delay_desc|lbl_ws|lbl_env|lbl_buf|lbl_avatar|lbl_broadcast|lbl_lane_broadcast|lbl_no_data|lbl_no_data_rtsp|lbl_no_data_ws
		//       |ch0_measured|ch0_total|ch0_offset|ch1_measured|ch1_total|ch1_offset|...
		bool parse_diagram_payload(const QString &text, DiagramData &out) {
			QStringList fields;
			if (!split_escaped_pipe_fields(text, fields))
				return false;
			if (fields.empty() || fields[0] != QLatin1String("DDIAGRAM"))
				return false;

			constexpr int kFixedFields = 6; // magic + R + A + buf + chCount + master_delay
			constexpr int kLabelFields = 11;
			if (fields.size() < kFixedFields + kLabelFields)
				return false;

			bool ok = false;
			out.R   = fields[1].toInt(&ok);
			if (!ok) return false;
			out.A = fields[2].toInt(&ok);
			if (!ok) return false;
			out.buf = fields[3].toInt(&ok);
			if (!ok) return false;
			out.ch_count = fields[4].toInt(&ok);
			if (!ok) return false;
			out.master_delay = fields[5].toInt(&ok);
			if (!ok) return false;

			out.lbl_delay          = fields[6];
			out.lbl_delay_desc     = fields[7];
			out.lbl_ws             = fields[8];
			out.lbl_env            = fields[9];
			out.lbl_buf            = fields[10];
			out.lbl_avatar         = fields[11];
			out.lbl_broadcast      = fields[12];
			out.lbl_lane_broadcast = fields[13];
			out.lbl_no_data        = fields[14];
			out.lbl_no_data_rtsp   = fields[15];
			out.lbl_no_data_ws     = fields[16];

			constexpr int kChFieldStart = kFixedFields + kLabelFields; // 17
			constexpr int kChFieldCount = 3;                           // measured_ms, total_ms, offset_ms
			if (fields.size() < kChFieldStart + out.ch_count * kChFieldCount)
				return false;

			out.channels.resize(static_cast<size_t>(out.ch_count));
			int idx = kChFieldStart;
			for (int i = 0; i < out.ch_count; ++i) {
				out.channels[i].measured_ms = fields[idx++].toFloat();
				out.channels[i].total_ms    = fields[idx++].toInt();
				out.channels[i].offset_ms   = fields[idx++].toInt();
			}
			return true;
		}

		// プレースホルダーを DelayDiagramWidget へ差し替える。
		void do_diagram_inject(void *param) {
			auto ctx = std::unique_ptr<DiagramInjectCtx>(
				static_cast<DiagramInjectCtx *>(param));
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
				if (text.startsWith(QLatin1String(kDiagramMagic)))
					found.push_back({lbl, text});
			}
			for (const auto &ph : found)
				collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

			int replaced_count = 0;
			for (const auto &ph : found) {
				DiagramData data;
				if (!parse_diagram_payload(ph.text, data))
					continue;

				QWidget *parent = ph.label->parentWidget();
				if (!parent) continue;
				auto *form = qobject_cast<QFormLayout *>(parent->layout());
				if (!form) continue;

				int                   row = -1;
				QFormLayout::ItemRole role;
				form->getWidgetPosition(ph.label, &row, &role);
				if (row < 0) continue;

				auto *widget = new DelayDiagramWidget(data, parent);

				form->removeRow(row);
				form->insertRow(row, widget);
				++replaced_count;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kDiagramInjectRetryMs,
								   [next]() { do_diagram_inject(next); });
				return;
			}
		}

	} // namespace

	// ============================================================
	// 公開 API
	// ============================================================

	obs_property_t *obs_properties_add_delay_diagram(
		obs_properties_t         *props,
		const char               *prop_name,
		const DelayDiagramInfo   &info,
		const DelayDiagramLabels &labels) {
		if (!props || !prop_name || !*prop_name)
			return nullptr;

		// 書式: DDIAGRAM|R|A|buf|chCount|master_delay
		//       |lbl_delay|lbl_delay_desc|lbl_ws|lbl_env|lbl_buf|lbl_avatar|lbl_broadcast|lbl_lane_broadcast|lbl_no_data|lbl_no_data_rtsp|lbl_no_data_ws
		//       |ch0_measured|ch0_total|...
		std::string payload = "DDIAGRAM";
		{
			char buf[64];
			std::snprintf(buf, sizeof(buf), "|%d|%d|%d|%d|%d", info.R, info.A, info.buf, info.ch_count, info.master_delay);
			payload += buf;
		}
		// 11 label fields
		for (const char *s : {
				 labels.legend_delay,
				 labels.legend_delay_desc,
				 labels.legend_ws,
				 labels.legend_env,
				 labels.legend_buf,
				 labels.legend_avatar,
				 labels.legend_broadcast,
				 labels.lane_broadcast,
				 labels.no_data,
				 labels.no_data_rtsp,
				 labels.no_data_ws}) {
			payload += '|';
			payload += escape_field(s ? s : "");
		}
		// per-channel fields (3 per channel)
		for (int i = 0; i < info.ch_count; ++i) {
			const auto &ch = info.channels[i];
			char        nums[3][32];
			std::snprintf(nums[0], sizeof(nums[0]), "%.6g", static_cast<double>(ch.measured_ms));
			std::snprintf(nums[1], sizeof(nums[1]), "%d", ch.total_ms);
			std::snprintf(nums[2], sizeof(nums[2]), "%d", ch.offset_ms);
			payload += '|';
			payload += nums[0];
			payload += '|';
			payload += nums[1];
			payload += '|';
			payload += nums[2];
		}

		return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
	}

	void schedule_delay_diagram_inject(obs_source_t *source) {
		if (!source) return;
		auto ctx = std::make_unique<DiagramInjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_diagram_inject, ctx.release(), false);
	}

} // namespace ods::widgets
