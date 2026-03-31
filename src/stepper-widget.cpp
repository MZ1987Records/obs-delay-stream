#include "stepper-widget.hpp"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr char kStepperMagic[] = "STEPPER\x1F";  // legacy format
constexpr char kStepperMagicPipe[] = "STEPPER|";  // current format
constexpr int kStepperInjectRetryMax = 12;
constexpr int kStepperInjectRetryMs = 25;

class StepperRow : public QWidget {
public:
    StepperRow(obs_source_t *source, const char *key, double min_val,
               double max_val, double def_val, int decimals,
               const char *suffix, bool store_as_int = false,
               QWidget *parent = nullptr)
        : QWidget(parent),
          source_(source ? obs_source_get_ref(source) : nullptr),
          key_(key),
          def_val_(def_val),
          store_as_int_(store_as_int)
    {
        auto *lay = new QHBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);

        reset_btn_ = new QPushButton(QStringLiteral("Reset"));
        reset_btn_->setStyleSheet(
            QStringLiteral("padding-left: 3px; padding-right: 3px;"));
        {
            const QFontMetrics fm(reset_btn_->font());
            const int w = fm.horizontalAdvance(QStringLiteral("Reset")) + 20;
            reset_btn_->setFixedWidth(w);
        }
        lay->addWidget(reset_btn_);

        spin_ = new QDoubleSpinBox();
        spin_->setRange(min_val, max_val);
        spin_->setDecimals(decimals);
        if (suffix && *suffix)
            spin_->setSuffix(QString::fromUtf8(suffix));
        spin_->setSingleStep(1.0);
        spin_->setKeyboardTracking(false);
        spin_->setStyleSheet(QStringLiteral(
            "QAbstractSpinBox { padding-left: 4px; padding-right: 4px; }"));
        spin_->setMinimumWidth(0);
        spin_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        lay->addWidget(spin_, 1);

        static constexpr int kDeltas[] = {-100, -10, 10, 100};
        const int delta_btn_w =
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

        connect(reset_btn_, &QPushButton::clicked, this,
                [this]() { spin_->setValue(def_val_); });

        connect(spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &StepperRow::onValueChanged);

        updateResetVisibility();
        loadFromSettings();
    }

    ~StepperRow() override
    {
        if (source_) {
            obs_source_release(source_);
            source_ = nullptr;
        }
    }

    void loadFromSettings()
    {
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
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        updateResetVisibility();
    }

    void updateResetVisibility()
    {
        if (!reset_btn_)
            return;
        auto *lay = qobject_cast<QHBoxLayout *>(layout());
        if (!lay)
            return;

        constexpr int kSpinMinWidthWithReset = 90;
        const int margins =
            lay->contentsMargins().left() + lay->contentsMargins().right();
        const int spacing = lay->spacing();
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

    void onValueChanged(double val)
    {
        if (!source_)
            return;
        obs_data_t *s = obs_source_get_settings(source_);
        if (s) {
            if (store_as_int_) {
                obs_data_set_int(s, key_.c_str(),
                                 static_cast<int>(std::lround(val)));
            } else {
                obs_data_set_double(s, key_.c_str(), val);
            }
            obs_source_update(source_, s);
            obs_data_release(s);
        }
    }

    obs_source_t *source_;
    std::string key_;
    double def_val_;
    bool store_as_int_ = false;
    QPushButton *reset_btn_ = nullptr;
    std::vector<QPushButton *> delta_buttons_;
    QDoubleSpinBox *spin_ = nullptr;
};

struct StepperInjectCtx {
    explicit StepperInjectCtx(obs_source_t *src)
        : source(src ? obs_source_get_ref(src) : nullptr)
    {
    }
    ~StepperInjectCtx()
    {
        if (source) {
            obs_source_release(source);
            source = nullptr;
        }
    }
    obs_source_t *source = nullptr;
    int retries_left = kStepperInjectRetryMax;
};

bool parse_stepper_payload(const QString &text, QStringList &fields)
{
    if (text.startsWith(QLatin1String(kStepperMagicPipe))) {
        fields = text.split(QChar('|'));
        return fields.size() >= 8;
    }
    if (text.startsWith(QLatin1String(kStepperMagic))) {
        fields = text.split(QChar('\x1F'));
        return fields.size() >= 8;
    }
    return false;
}

void do_stepper_inject(void *param)
{
    auto *ctx = static_cast<StepperInjectCtx *>(param);
    if (!ctx)
        return;

    struct Placeholder {
        QLabel *label;
        QString text;
    };
    std::vector<Placeholder> found;

    const auto all_widgets = QApplication::allWidgets();
    for (QWidget *w : all_widgets) {
        auto *lbl = qobject_cast<QLabel *>(w);
        if (!lbl)
            continue;
        const QString text = lbl->text();
        if (text.startsWith(QLatin1String("STEPPER")))
            found.push_back({lbl, text});
    }

    int replaced_count = 0;
    for (auto &ph : found) {
        QStringList fields;
        if (!parse_stepper_payload(ph.text, fields))
            continue;

        const QString key = fields[1];
        const double min_val = fields[2].toDouble();
        const double max_val = fields[3].toDouble();
        const double def_val = fields[4].toDouble();
        const int decimals = fields[5].toInt();
        const QString suffix = fields[6];
        const bool store_as_int =
            (fields.size() >= 9 && fields[7] == QLatin1String("1"));
        const QString row_label =
            (fields.size() >= 9) ? fields.mid(8).join(QStringLiteral(" "))
                                 : fields.mid(7).join(QStringLiteral(" "));

        QWidget *parent = ph.label->parentWidget();
        if (!parent)
            continue;
        auto *form = qobject_cast<QFormLayout *>(parent->layout());
        if (!form)
            continue;

        int row = -1;
        QFormLayout::ItemRole role;
        form->getWidgetPosition(ph.label, &row, &role);
        if (row < 0)
            continue;

        auto *stepper = new StepperRow(
            ctx->source, key.toUtf8().constData(), min_val, max_val, def_val,
            decimals, suffix.toUtf8().constData(), store_as_int, parent);

        form->removeRow(row);
        if (!row_label.isEmpty())
            form->insertRow(row, row_label, stepper);
        else
            form->insertRow(row, stepper);
        ++replaced_count;
    }

    if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
        ctx->retries_left > 0) {
        --ctx->retries_left;
        QTimer::singleShot(kStepperInjectRetryMs,
                           [ctx]() { do_stepper_inject(ctx); });
        return;
    }

    delete ctx;
}

} // namespace

obs_property_t *obs_properties_add_stepper(obs_properties_t *props,
                                           const char *prop_name,
                                           const char *label,
                                           const char *setting_key,
                                           double min_val,
                                           double max_val,
                                           double def_val,
                                           int decimals,
                                           const char *suffix,
                                           bool store_as_int)
{
    const int len = std::snprintf(nullptr, 0,
                                  "STEPPER|%s|%.10g|%.10g|%.10g|%d|%s|%d|%s",
                                  setting_key, min_val, max_val, def_val,
                                  decimals, suffix ? suffix : "",
                                  store_as_int ? 1 : 0, label ? label : "");
    if (len < 0)
        return nullptr;

    std::string buf(static_cast<size_t>(len) + 1, '\0');
    std::snprintf(buf.data(), buf.size(),
                  "STEPPER|%s|%.10g|%.10g|%.10g|%d|%s|%d|%s", setting_key,
                  min_val, max_val, def_val, decimals, suffix ? suffix : "",
                  store_as_int ? 1 : 0, label ? label : "");
    return obs_properties_add_text(props, prop_name, buf.c_str(), OBS_TEXT_INFO);
}

void schedule_stepper_inject(obs_source_t *source)
{
    if (!source)
        return;
    auto *ctx = new StepperInjectCtx(source);
    obs_queue_task(OBS_TASK_UI, do_stepper_inject, ctx, false);
}
