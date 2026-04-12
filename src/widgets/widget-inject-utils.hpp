#pragma once

#include "core/constants.hpp"

#include <QAbstractScrollArea>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QWidget>
#include <vector>

namespace ods::widgets {

	/**
	 * 祖先スクロール領域のスナップショット。
	 */
	struct ScrollSnapshot {
		QPointer<QAbstractScrollArea> area;      ///< 対象スクロール領域
		int                           value = 0; ///< 保存したスクロール位置
	};

	// 最も近い祖先スクロール領域の位置を記録する。
	inline void collect_ancestor_scroll_snapshot(QWidget                     *start,
												 std::vector<ScrollSnapshot> &snapshots) {
		QWidget *cur = start;
		while (cur) {
			auto *area = qobject_cast<QAbstractScrollArea *>(cur);
			if (area && area->verticalScrollBar()) {
				for (const auto &snap : snapshots) {
					if (snap.area == area)
						return;
				}
				snapshots.push_back({QPointer<QAbstractScrollArea>(area),
									 area->verticalScrollBar()->value()});
				return;
			}
			cur = cur->parentWidget();
		}
	}

	// 記録したスクロール位置を復元する。
	inline void restore_scroll_snapshots(const std::vector<ScrollSnapshot> &snapshots) {
		for (const auto &snap : snapshots) {
			auto *area = snap.area.data();
			if (!area || !area->verticalScrollBar())
				continue;
			area->verticalScrollBar()->setValue(snap.value);
			const int                     restore_value = snap.value;
			QPointer<QAbstractScrollArea> area_ptr(area);
			QTimer::singleShot(0, area, [area_ptr, restore_value]() {
				if (!area_ptr || !area_ptr->verticalScrollBar())
					return;
				area_ptr->verticalScrollBar()->setValue(restore_value);
			});
		}
	}

	/// パレットのテーマ（明/暗）に合った警告テキスト色を返す。
	inline QColor warningTextColor(const QPalette &pal) {
		const bool isDark = (pal.color(QPalette::Window).lightnessF() < 0.5);
		return isDark ? QColor(ods::core::UI_COLOR_WARNING_DARK)
					  : QColor(ods::core::UI_COLOR_WARNING_LIGHT);
	}

	/// 色付き四角マーク＋テキストのフォームラベル用ウィジェットを生成する。
	inline QWidget *create_colored_label(const QString &text, const QColor &color, QWidget *parent = nullptr) {
		auto *widget = new QWidget(parent);
		auto *lay    = new QHBoxLayout(widget);
		lay->setContentsMargins(0, 0, 0, 0);
		lay->setSpacing(4);

		auto *swatch = new QFrame(widget);
		swatch->setFixedSize(10, 10);
		swatch->setStyleSheet(
			QStringLiteral("background-color: %1; border-radius: 2px;").arg(color.name(QColor::HexRgb)));
		lay->addWidget(swatch);

		auto *lbl = new QLabel(text, widget);
		lay->addWidget(lbl);
		lay->addStretch(1);

		return widget;
	}

} // namespace ods::widgets
