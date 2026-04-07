#pragma once

#include <QDoubleSpinBox>
#include <QWheelEvent>

namespace ods::widgets {

	/**
	 * マウスオーバーだけではホイールで値が変わらないスピンボックス。
	 *
	 * クリックでフォーカスを得た後のみホイール操作を受け付ける。
	 */
	class FocusSpinBox : public QDoubleSpinBox {
	public:

		explicit FocusSpinBox(QWidget *parent = nullptr) : QDoubleSpinBox(parent) {
			// Qt::WheelFocus だとホイール操作で自動フォーカスされるため無効化する。
			setFocusPolicy(Qt::StrongFocus);
		}

	protected:

		void wheelEvent(QWheelEvent *event) override {
			event->ignore();
		}
	};

} // namespace ods::widgets
