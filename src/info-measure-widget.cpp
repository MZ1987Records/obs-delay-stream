#include "info-measure-widget.hpp"
#include "widget-inject-utils.hpp"
#include "widget-payload-utils.hpp"

#include <QApplication>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr char kInfoMeasureMagicPipe[] = "INFOMEAS|";
constexpr int kInfoMeasureInjectRetryMax = 40;
constexpr int kInfoMeasureInjectRetryMs = 5;

std::unordered_map<std::string, obs_property_t*> g_info_measure_action_props;
std::mutex g_info_measure_action_props_mutex;
std::atomic<uint64_t> g_info_measure_binding_seq{1};

std::string make_info_measure_action_name(const char* prop_name)
{
    std::string action_name = prop_name ? prop_name : "";
    action_name += "__infomeas_action";
    return action_name;
}

std::string make_info_measure_binding_id(const std::string& action_name)
{
    const uint64_t seq =
        g_info_measure_binding_seq.fetch_add(1, std::memory_order_relaxed);
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "#%llu",
                  static_cast<unsigned long long>(seq));
    return action_name + suffix;
}

using widget_payload::escape_field;
using widget_payload::split_escaped_pipe_fields;

class InfoMeasureRow : public QWidget {
public:
    InfoMeasureRow(obs_source_t* source, const char* binding_id,
                   const char* info_text, const char* result_text,
                   const char* button_label, bool button_enabled,
                   bool info_warn, int ch_index,
                   QWidget* parent = nullptr)
        : QWidget(parent),
          source_(source ? obs_source_get_ref(source) : nullptr),
          binding_id_(binding_id ? binding_id : ""),
          ch_index_(ch_index),
          info_warn_(info_warn)
    {
        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);

        info_label_ = new QLabel(QString::fromUtf8(info_text ? info_text : ""));
        applyWarnStyle(info_warn);
        info_label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        lay->addWidget(info_label_, 1);

        button_ = new QPushButton(QString::fromUtf8(button_label ? button_label : ""));
        button_->setStyleSheet(
            QStringLiteral("padding-left: 3px; padding-right: 3px;"));
        updateButtonWidth();
        button_->setEnabled(button_enabled);
        connect(button_, &QPushButton::clicked, this, &InfoMeasureRow::onButtonClicked);
        lay->addWidget(button_);

        result_label_ = new QLabel(QString::fromUtf8(result_text ? result_text : ""));
        result_label_->setVisible(!result_label_->text().isEmpty());
        lay->addWidget(result_label_);

        if (source_ && ch_index_ >= 0) {
            poll_timer_ = new QTimer(this);
            poll_timer_->setInterval(250);
            connect(poll_timer_, &QTimer::timeout, this, &InfoMeasureRow::pollWarnState);
            poll_timer_->start();
        }
    }

    ~InfoMeasureRow() override
    {
        {
            std::lock_guard<std::mutex> lock(g_info_measure_action_props_mutex);
            g_info_measure_action_props.erase(binding_id_);
        }
        if (source_) {
            obs_source_release(source_);
            source_ = nullptr;
        }
    }

private:
    bool event(QEvent* e) override
    {
        switch (e->type()) {
        case QEvent::FontChange:
        case QEvent::StyleChange:
        case QEvent::Polish:
            updateButtonWidth();
            break;
        default:
            break;
        }
        return QWidget::event(e);
    }

    void updateButtonWidth()
    {
        if (!button_)
            return;
        const int width = button_->fontMetrics().horizontalAdvance(button_->text()) + 20;
        button_->setFixedWidth(width);
    }

    void onButtonClicked()
    {
        obs_property_t* action_prop = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_info_measure_action_props_mutex);
            auto it = g_info_measure_action_props.find(binding_id_);
            if (it != g_info_measure_action_props.end())
                action_prop = it->second;
        }
        if (!action_prop)
            return;

        const bool need_refresh = obs_property_button_clicked(action_prop, source_);
        if (need_refresh && source_) {
            obs_source_update_properties(source_);
        }
    }

    void applyWarnStyle(bool warn)
    {
        if (!info_label_)
            return;
        if (warn)
            info_label_->setStyleSheet(QStringLiteral("color: #ff4444; font-weight: bold;"));
        else
            info_label_->setStyleSheet(QString());
    }

    void pollWarnState()
    {
        if (!source_)
            return;
        obs_data_t* s = obs_source_get_settings(source_);
        if (!s)
            return;
        char key[32];
        std::snprintf(key, sizeof(key), "sub%d_delay_ms", ch_index_);
        const float base = static_cast<float>(obs_data_get_double(s, key));
        std::snprintf(key, sizeof(key), "sub%d_adjust_ms", ch_index_);
        const float adjust = static_cast<float>(obs_data_get_double(s, key));
        const float offset = static_cast<float>(obs_data_get_double(s, "sub_offset_ms"));
        obs_data_release(s);

        const bool warn = (base + adjust + offset) < 0.0f;
        if (warn != info_warn_) {
            info_warn_ = warn;
            applyWarnStyle(warn);
        }
    }

    obs_source_t* source_ = nullptr;
    std::string binding_id_;
    int ch_index_ = -1;
    bool info_warn_ = false;
    QTimer* poll_timer_ = nullptr;
    QLabel* info_label_ = nullptr;
    QPushButton* button_ = nullptr;
    QLabel* result_label_ = nullptr;
};

struct InfoMeasureInjectCtx {
    explicit InfoMeasureInjectCtx(obs_source_t* src)
        : source(src ? obs_source_get_ref(src) : nullptr)
    {
    }
    ~InfoMeasureInjectCtx()
    {
        if (source) {
            obs_source_release(source);
            source = nullptr;
        }
    }
    obs_source_t* source = nullptr;
    int retries_left = kInfoMeasureInjectRetryMax;
};

bool parse_info_measure_payload(const QString& text, QStringList& fields)
{
    if (!split_escaped_pipe_fields(text, fields))
        return false;
    if (fields.empty() || fields[0] != QLatin1String("INFOMEAS"))
        return false;
    return fields.size() >= 9;
}

void do_info_measure_inject(void* param)
{
    auto* ctx = static_cast<InfoMeasureInjectCtx*>(param);
    if (!ctx)
        return;

    struct Placeholder {
        QLabel* label = nullptr;
        QString text;
    };
    std::vector<Placeholder> found;
    std::vector<widget_inject::ScrollSnapshot> scroll_snapshots;

    const auto all_widgets = QApplication::allWidgets();
    for (QWidget* w : all_widgets) {
        auto* lbl = qobject_cast<QLabel*>(w);
        if (!lbl)
            continue;
        const QString text = lbl->text();
        if (text.startsWith(QLatin1String(kInfoMeasureMagicPipe)))
            found.push_back({lbl, text});
    }
    for (const auto& ph : found)
        widget_inject::collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

    int replaced_count = 0;
    for (const auto& ph : found) {
        QStringList fields;
        if (!parse_info_measure_payload(ph.text, fields))
            continue;

        const QString binding_id = fields[1];
        const QString info_text = fields[2];
        const QString result_text = fields[3];
        const QString button_label = fields[4];
        const bool button_enabled = (fields[5] == QLatin1String("1"));
        const bool info_warn = (fields[6] == QLatin1String("1"));
        const int ch_index = fields[7].toInt();
        const QString row_label = fields.mid(8).join(QStringLiteral(" "));

        QWidget* parent = ph.label->parentWidget();
        if (!parent)
            continue;
        auto* form = qobject_cast<QFormLayout*>(parent->layout());
        if (!form)
            continue;

        int row = -1;
        QFormLayout::ItemRole role;
        form->getWidgetPosition(ph.label, &row, &role);
        if (row < 0)
            continue;

        auto* row_widget = new InfoMeasureRow(
            ctx->source, binding_id.toUtf8().constData(),
            info_text.toUtf8().constData(), result_text.toUtf8().constData(),
            button_label.toUtf8().constData(), button_enabled, info_warn,
            ch_index, parent);

        form->removeRow(row);
        if (!row_label.isEmpty())
            form->insertRow(row, row_label, row_widget);
        else
            form->insertRow(row, row_widget);
        ++replaced_count;
    }

    widget_inject::restore_scroll_snapshots(scroll_snapshots);

    if ((found.empty() || replaced_count < static_cast<int>(found.size())) &&
        ctx->retries_left > 0) {
        --ctx->retries_left;
        QTimer::singleShot(kInfoMeasureInjectRetryMs,
                           [ctx]() { do_info_measure_inject(ctx); });
        return;
    }

    delete ctx;
}

} // namespace

obs_property_t* obs_properties_add_info_measure(
    obs_properties_t* props,
    const char* prop_name,
    const char* label,
    const char* info_text,
    const char* result_text,
    const char* button_label,
    obs_property_clicked_t clicked,
    void* clicked_priv,
    bool button_enabled,
    bool info_warn,
    int ch_index)
{
    if (!props || !prop_name || !*prop_name || !button_label || !*button_label ||
        !clicked) {
        return nullptr;
    }

    const std::string action_name = make_info_measure_action_name(prop_name);
    if (obs_properties_get(props, prop_name) ||
        obs_properties_get(props, action_name.c_str())) {
        return nullptr;
    }

    obs_property_t* action_prop = obs_properties_add_button2(
        props, action_name.c_str(), button_label, clicked, clicked_priv);
    if (!action_prop)
        return nullptr;
    obs_property_set_visible(action_prop, false);

    const std::string binding_id = make_info_measure_binding_id(action_name);
    {
        std::lock_guard<std::mutex> lock(g_info_measure_action_props_mutex);
        g_info_measure_action_props[binding_id] = action_prop;
    }

    char ch_index_str[12];
    std::snprintf(ch_index_str, sizeof(ch_index_str), "%d", ch_index);
    const std::string payload =
        std::string("INFOMEAS|") +
        escape_field(binding_id.c_str()) + "|" +
        escape_field(info_text) + "|" +
        escape_field(result_text) + "|" +
        escape_field(button_label) + "|" +
        (button_enabled ? "1" : "0") + "|" +
        (info_warn ? "1" : "0") + "|" +
        ch_index_str + "|" +
        escape_field(label ? label : "");
    return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
}

void schedule_info_measure_inject(obs_source_t* source)
{
    if (!source)
        return;
    auto* ctx = new InfoMeasureInjectCtx(source);
    obs_queue_task(OBS_TASK_UI, do_info_measure_inject, ctx, false);
}
