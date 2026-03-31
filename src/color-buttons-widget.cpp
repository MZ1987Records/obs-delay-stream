#include "color-buttons-widget.hpp"
#include "widget-inject-utils.hpp"

#include <QApplication>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr char kColorButtonRowMagicPipe[] = "CBTNROW|";
constexpr int kColorButtonRowInjectRetryMax = 40;
constexpr int kColorButtonRowInjectRetryMs = 5;

struct ParsedButtonSpec {
    QString label;
    bool enabled = true;
    QString bg_color;
    QString text_color;
};

std::unordered_map<std::string, std::vector<obs_property_t*>> g_color_button_action_props;
std::mutex g_color_button_action_props_mutex;
std::atomic<uint64_t> g_color_button_binding_seq{1};

QString make_button_style_sheet(const QString& bg_color,
                                const QString& text_color)
{
    QStringList styles;
    styles.push_back(QStringLiteral(
        "QPushButton { padding-left: 3px; padding-right: 3px; }"));

    QStringList enabled_styles;
    if (!bg_color.isEmpty())
        enabled_styles.push_back(QStringLiteral("background-color: ") + bg_color);
    if (!text_color.isEmpty())
        enabled_styles.push_back(QStringLiteral("color: ") + text_color);
    if (!enabled_styles.isEmpty()) {
        styles.push_back(QStringLiteral("QPushButton:enabled { ") +
                         enabled_styles.join(QStringLiteral("; ")) +
                         QStringLiteral("; }"));
    }
    return styles.join(QStringLiteral(" "));
}

std::string escape_payload_field(const char* src)
{
    std::string out;
    if (!src)
        return out;
    while (*src) {
        const char c = *src++;
        if (c == '\\' || c == '|')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

bool split_escaped_pipe_fields(const QString& text, QStringList& fields)
{
    fields.clear();
    QString cur;
    bool escaped = false;
    for (QChar ch : text) {
        if (escaped) {
            cur.append(ch);
            escaped = false;
            continue;
        }
        if (ch == QChar('\\')) {
            escaped = true;
            continue;
        }
        if (ch == QChar('|')) {
            fields.push_back(cur);
            cur.clear();
            continue;
        }
        cur.append(ch);
    }
    if (escaped)
        cur.append(QChar('\\'));
    fields.push_back(cur);
    return true;
}

std::string make_color_button_binding_id(const char* prop_name)
{
    std::string base = prop_name ? prop_name : "";
    const uint64_t seq =
        g_color_button_binding_seq.fetch_add(1, std::memory_order_relaxed);
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "#%llu",
                  static_cast<unsigned long long>(seq));
    return base + suffix;
}

class ColorButtonRow : public QWidget {
public:
    ColorButtonRow(obs_source_t* source, const char* binding_id,
                   const std::vector<ParsedButtonSpec>& buttons,
                   QWidget* parent = nullptr)
        : QWidget(parent),
          source_(source ? obs_source_get_ref(source) : nullptr),
          binding_id_(binding_id ? binding_id : "")
    {
        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);

        for (size_t i = 0; i < buttons.size(); ++i) {
            const auto& spec = buttons[i];
            auto* button = new QPushButton(spec.label);
            button->setStyleSheet(
                make_button_style_sheet(spec.bg_color, spec.text_color));
            button->setEnabled(spec.enabled);
            connect(button, &QPushButton::clicked, this, [this, i]() {
                onButtonClicked(i);
            });
            lay->addWidget(button);
            buttons_.push_back(button);
        }
        // 余白側へストレッチを入れて、固定幅ボタン群を常に左寄せにする。
        lay->addStretch(1);
        updateButtonWidths();
        QTimer::singleShot(0, this, [this]() { updateButtonWidths(); });
    }

    ~ColorButtonRow() override
    {
        {
            std::lock_guard<std::mutex> lock(g_color_button_action_props_mutex);
            g_color_button_action_props.erase(binding_id_);
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
            updateButtonWidths();
            break;
        default:
            break;
        }
        return QWidget::event(e);
    }

    void updateButtonWidths()
    {
        for (auto* button : buttons_) {
            if (!button)
                continue;
            const int width =
                button->fontMetrics().horizontalAdvance(button->text()) + 20;
            button->setFixedWidth(width);
        }
    }

    void onButtonClicked(size_t index)
    {
        obs_property_t* action_prop = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_color_button_action_props_mutex);
            auto it = g_color_button_action_props.find(binding_id_);
            if (it != g_color_button_action_props.end() &&
                index < it->second.size()) {
                action_prop = it->second[index];
            }
        }
        if (!action_prop)
            return;

        const bool need_refresh = obs_property_button_clicked(action_prop, source_);
        if (need_refresh && source_) {
            obs_source_update_properties(source_);
        }
    }

    obs_source_t* source_ = nullptr;
    std::string binding_id_;
    std::vector<QPushButton*> buttons_;
};

struct ColorButtonInjectCtx {
    explicit ColorButtonInjectCtx(obs_source_t* src)
        : source(src ? obs_source_get_ref(src) : nullptr)
    {
    }
    ~ColorButtonInjectCtx()
    {
        if (source) {
            obs_source_release(source);
            source = nullptr;
        }
    }
    obs_source_t* source = nullptr;
    int retries_left = kColorButtonRowInjectRetryMax;
};

bool parse_bool_field(const QString& value)
{
    return value == QLatin1String("1");
}

bool parse_color_button_payload(const QString& text,
                                QString& binding_id,
                                QString& row_label,
                                std::vector<ParsedButtonSpec>& buttons)
{
    QStringList fields;
    if (!split_escaped_pipe_fields(text, fields))
        return false;
    if (fields.size() < 4 || fields[0] != QLatin1String("CBTNROW"))
        return false;

    bool ok = false;
    const int count = fields[3].toInt(&ok);
    if (!ok || count <= 0)
        return false;

    const int expected_fields = 4 + count * 4;
    if (fields.size() < expected_fields)
        return false;

    binding_id = fields[1];
    row_label = fields[2];
    buttons.clear();
    buttons.reserve(static_cast<size_t>(count));
    int idx = 4;
    for (int i = 0; i < count; ++i) {
        ParsedButtonSpec spec;
        spec.label = fields[idx++];
        spec.enabled = parse_bool_field(fields[idx++]);
        spec.bg_color = fields[idx++];
        spec.text_color = fields[idx++];
        buttons.push_back(spec);
    }
    return true;
}

void do_color_button_row_inject(void* param)
{
    auto* ctx = static_cast<ColorButtonInjectCtx*>(param);
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
        if (text.startsWith(QLatin1String(kColorButtonRowMagicPipe)))
            found.push_back({lbl, text});
    }
    for (const auto& ph : found)
        widget_inject::collect_ancestor_scroll_snapshot(ph.label, scroll_snapshots);

    int replaced_count = 0;
    for (const auto& ph : found) {
        QString binding_id;
        QString row_label;
        std::vector<ParsedButtonSpec> buttons;
        if (!parse_color_button_payload(ph.text, binding_id, row_label, buttons))
            continue;

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

        auto* row_widget = new ColorButtonRow(
            ctx->source, binding_id.toUtf8().constData(), buttons, parent);

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
        QTimer::singleShot(kColorButtonRowInjectRetryMs,
                           [ctx]() { do_color_button_row_inject(ctx); });
        return;
    }

    delete ctx;
}

} // namespace

obs_property_t* obs_properties_add_color_button_row(
    obs_properties_t* props,
    const char* prop_name,
    const char* label,
    const ObsColorButtonSpec* buttons,
    size_t button_count)
{
    if (!props || !prop_name || !*prop_name || !buttons || button_count == 0)
        return nullptr;
    if (obs_properties_get(props, prop_name))
        return nullptr;

    std::unordered_set<std::string> action_names;
    action_names.reserve(button_count);
    for (size_t i = 0; i < button_count; ++i) {
        const auto& spec = buttons[i];
        if (!spec.action_prop_name || !*spec.action_prop_name ||
            !spec.button_label || !*spec.button_label ||
            !spec.clicked) {
            return nullptr;
        }
        const std::string action_name(spec.action_prop_name);
        if (action_name == prop_name)
            return nullptr;
        if (obs_properties_get(props, action_name.c_str()))
            return nullptr;
        if (!action_names.insert(action_name).second)
            return nullptr;
    }

    std::vector<obs_property_t*> action_props;
    action_props.reserve(button_count);
    for (size_t i = 0; i < button_count; ++i) {
        const auto& spec = buttons[i];
        obs_property_t* action_prop = obs_properties_add_button2(
            props, spec.action_prop_name, spec.button_label,
            spec.clicked, spec.clicked_priv);
        if (!action_prop)
            return nullptr;
        obs_property_set_visible(action_prop, false);
        obs_property_set_enabled(action_prop, spec.enabled);
        action_props.push_back(action_prop);
    }

    const std::string binding_id = make_color_button_binding_id(prop_name);

    std::string payload = "CBTNROW|";
    payload += escape_payload_field(binding_id.c_str());
    payload += "|";
    payload += escape_payload_field(label ? label : "");
    payload += "|";
    payload += std::to_string(button_count);

    for (size_t i = 0; i < button_count; ++i) {
        const auto& spec = buttons[i];
        payload += "|";
        payload += escape_payload_field(spec.button_label);
        payload += "|";
        payload += (spec.enabled ? "1" : "0");
        payload += "|";
        payload += escape_payload_field(spec.bg_color ? spec.bg_color : "");
        payload += "|";
        payload += escape_payload_field(spec.text_color ? spec.text_color : "");
    }

    obs_property_t* placeholder =
        obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
    if (!placeholder)
        return nullptr;

    {
        std::lock_guard<std::mutex> lock(g_color_button_action_props_mutex);
        g_color_button_action_props[binding_id] = action_props;
    }
    return placeholder;
}

void schedule_color_button_row_inject(obs_source_t* source)
{
    if (!source)
        return;
    auto* ctx = new ColorButtonInjectCtx(source);
    obs_queue_task(OBS_TASK_UI, do_color_button_row_inject, ctx, false);
}
