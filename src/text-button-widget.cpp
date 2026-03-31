#include "text-button-widget.hpp"
#include "widget-inject-utils.hpp"
#include "widget-payload-utils.hpp"

#include <QApplication>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
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

constexpr char kTextButtonMagicPipe[] = "TEXTBTN|";
constexpr int kTextButtonInjectRetryMax = 40;
constexpr int kTextButtonInjectRetryMs = 5;

std::unordered_map<std::string, obs_property_t *> g_text_button_action_props;
std::mutex g_text_button_action_props_mutex;
std::atomic<uint64_t> g_text_button_binding_seq{1};

std::string make_text_button_action_name(const char *prop_name)
{
    std::string action_name = prop_name ? prop_name : "";
    action_name += "__textbtn_action";
    return action_name;
}

std::string make_text_button_binding_id(const std::string &action_name)
{
    const uint64_t seq =
        g_text_button_binding_seq.fetch_add(1, std::memory_order_relaxed);
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "#%llu",
                  static_cast<unsigned long long>(seq));
    return action_name + suffix;
}

using widget_payload::escape_field;
using widget_payload::split_escaped_pipe_fields;

class TextButtonRow : public QWidget {
public:
    TextButtonRow(obs_source_t *source, const char *key, const char *binding_id,
                  const char *button_label,
                  bool input_enabled, bool button_enabled,
                  QWidget *parent = nullptr)
        : QWidget(parent),
          source_(source ? obs_source_get_ref(source) : nullptr),
          key_(key ? key : ""),
          binding_id_(binding_id ? binding_id : "")
    {
        auto *lay = new QHBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);

        edit_ = new QLineEdit();
        edit_->setMinimumWidth(0);
        edit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        lay->addWidget(edit_, 1);

        button_ = new QPushButton(QString::fromUtf8(button_label ? button_label : ""));
        button_->setStyleSheet(
            QStringLiteral("padding-left: 3px; padding-right: 3px;"));
        updateButtonWidth();
        lay->addWidget(button_);

        connect(edit_, &QLineEdit::editingFinished, this,
                &TextButtonRow::onEditingFinished);
        connect(button_, &QPushButton::clicked, this,
                &TextButtonRow::onButtonClicked);

        edit_->setEnabled(input_enabled);
        button_->setEnabled(button_enabled);
        loadFromSettings();
        QTimer::singleShot(0, this, [this]() { updateButtonWidth(); });
    }

    ~TextButtonRow() override
    {
        std::lock_guard<std::mutex> lock(g_text_button_action_props_mutex);
        g_text_button_action_props.erase(binding_id_);
        if (source_) {
            obs_source_release(source_);
            source_ = nullptr;
        }
    }

private:
    bool event(QEvent *e) override
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

    void loadFromSettings()
    {
        if (!source_ || !edit_)
            return;
        obs_data_t *s = obs_source_get_settings(source_);
        if (!s)
            return;
        const char *text = obs_data_get_string(s, key_.c_str());
        obs_data_release(s);
        edit_->blockSignals(true);
        edit_->setText(QString::fromUtf8(text ? text : ""));
        edit_->blockSignals(false);
    }

    void syncToSettings()
    {
        if (!source_ || !edit_)
            return;
        obs_data_t *s = obs_source_get_settings(source_);
        if (!s)
            return;
        const QByteArray utf8 = edit_->text().toUtf8();
        obs_data_set_string(s, key_.c_str(), utf8.constData());
        obs_source_update(source_, s);
        obs_data_release(s);
    }

    void onEditingFinished()
    {
        syncToSettings();
    }

    void onButtonClicked()
    {
        syncToSettings();

        obs_property_t *action_prop = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_text_button_action_props_mutex);
            auto it = g_text_button_action_props.find(binding_id_);
            if (it != g_text_button_action_props.end())
                action_prop = it->second;
        }
        if (!action_prop)
            return;

        const bool need_refresh = obs_property_button_clicked(action_prop, source_);
        if (need_refresh && source_) {
            obs_source_update_properties(source_);
        }
    }

    obs_source_t *source_ = nullptr;
    std::string key_;
    std::string binding_id_;
    QLineEdit *edit_ = nullptr;
    QPushButton *button_ = nullptr;
};

struct TextButtonInjectCtx {
    explicit TextButtonInjectCtx(obs_source_t *src)
        : source(src ? obs_source_get_ref(src) : nullptr)
    {
    }
    ~TextButtonInjectCtx()
    {
        if (source) {
            obs_source_release(source);
            source = nullptr;
        }
    }
    obs_source_t *source = nullptr;
    int retries_left = kTextButtonInjectRetryMax;
};

bool parse_text_button_payload(const QString &text, QStringList &fields)
{
    if (!split_escaped_pipe_fields(text, fields))
        return false;
    if (fields.empty() || fields[0] != QLatin1String("TEXTBTN"))
        return false;
    return fields.size() >= 5;
}

void do_text_button_inject(void *param)
{
    auto *ctx = static_cast<TextButtonInjectCtx *>(param);
    if (!ctx)
        return;

    struct Placeholder {
        QLabel *label;
        QString text;
    };
    std::vector<Placeholder> found;
    std::vector<widget_inject::ScrollSnapshot> scroll_snapshots;

    const auto all_widgets = QApplication::allWidgets();
    for (QWidget *w : all_widgets) {
        auto *lbl = qobject_cast<QLabel *>(w);
        if (!lbl)
            continue;
        const QString text = lbl->text();
        if (text.startsWith(QLatin1String(kTextButtonMagicPipe)))
            found.push_back({lbl, text});
    }
    for (const auto &ph : found)
        widget_inject::collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

    int replaced_count = 0;
    for (auto &ph : found) {
        QStringList fields;
        if (!parse_text_button_payload(ph.text, fields))
            continue;

        const QString key = fields[1];
        const QString binding_id = fields[2];
        const QString button_label = fields[3];
        bool input_enabled = true;
        bool button_enabled = true;
        QString row_label;
        if (fields.size() >= 7) {
            input_enabled = (fields[4] == QLatin1String("1"));
            button_enabled = (fields[5] == QLatin1String("1"));
            row_label = fields.mid(6).join(QStringLiteral(" "));
        } else if (fields.size() >= 6) {
            const bool enabled = (fields[4] == QLatin1String("1"));
            input_enabled = enabled;
            button_enabled = enabled;
            row_label = fields.mid(5).join(QStringLiteral(" "));
        } else {
            row_label = fields.mid(4).join(QStringLiteral(" "));
        }

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

        auto *row_widget = new TextButtonRow(
            ctx->source, key.toUtf8().constData(), binding_id.toUtf8().constData(),
            button_label.toUtf8().constData(),
            input_enabled, button_enabled, parent);

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
        QTimer::singleShot(kTextButtonInjectRetryMs,
                           [ctx]() { do_text_button_inject(ctx); });
        return;
    }

    delete ctx;
}

} // namespace

obs_property_t *obs_properties_add_text_button(obs_properties_t *props,
                                               const char *prop_name,
                                               const char *label,
                                               const char *setting_key,
                                               const char *button_label,
                                               obs_property_clicked_t clicked,
                                               void *clicked_priv,
                                               bool input_enabled,
                                               bool button_enabled)
{
    if (!props || !prop_name || !*prop_name || !setting_key || !*setting_key ||
        !button_label || !*button_label || !clicked) {
        return nullptr;
    }

    const std::string action_name = make_text_button_action_name(prop_name);
    if (obs_properties_get(props, prop_name) ||
        obs_properties_get(props, action_name.c_str())) {
        return nullptr;
    }

    obs_property_t *action_prop = obs_properties_add_button2(
        props, action_name.c_str(), button_label, clicked, clicked_priv);
    if (!action_prop)
        return nullptr;
    obs_property_set_visible(action_prop, false);

    const std::string binding_id = make_text_button_binding_id(action_name);
    {
        std::lock_guard<std::mutex> lock(g_text_button_action_props_mutex);
        g_text_button_action_props[binding_id] = action_prop;
    }

    const std::string payload =
        std::string("TEXTBTN|") +
        escape_field(setting_key) + "|" +
        escape_field(binding_id.c_str()) + "|" +
        escape_field(button_label) + "|" +
        (input_enabled ? "1" : "0") + std::string("|") +
        (button_enabled ? "1" : "0") + "|" +
        escape_field(label ? label : "");
    return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
}

void schedule_text_button_inject(obs_source_t *source)
{
    if (!source)
        return;
    auto *ctx = new TextButtonInjectCtx(source);
    obs_queue_task(OBS_TASK_UI, do_text_button_inject, ctx, false);
}
