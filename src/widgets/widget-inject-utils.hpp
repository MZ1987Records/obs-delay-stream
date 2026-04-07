#pragma once

#include <QAbstractScrollArea>
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

} // namespace ods::widgets
