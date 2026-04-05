#pragma once

#include <QDoubleSpinBox>
#include <QWheelEvent>

// マウスオーバーだけではホイールで値が変わらないスピンボックス。
// クリックしてフォーカスを得た後のみホイール操作を受け付ける。
class FocusSpinBox : public QDoubleSpinBox {
public:
    explicit FocusSpinBox(QWidget *parent = nullptr) : QDoubleSpinBox(parent)
    {
        // デフォルトの Qt::WheelFocus だとホイール操作で自動的にフォーカスが
        // 与えられ、hasFocus() チェックが常に true になるため無効化する
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }
};
