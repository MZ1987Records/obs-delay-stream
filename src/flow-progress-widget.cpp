#include "flow-progress-widget.hpp"
#include "widget-inject-utils.hpp"

#include <QApplication>
#include <QFormLayout>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

// プレースホルダーQLabel に埋め込むマジックプレフィックス。
// フォーマット: "FLOWPROG|{value}|{row_label}"
constexpr char kMagic[] = "FLOWPROG|";
constexpr int kInjectRetryMax = 40;
constexpr int kInjectRetryMs  = 5;

// ソース → QProgressBar の弱参照レジストリ。
// inject 後に登録し、update_flow_progress で直接 setValue() するために使う。
// QPointer はウィジェット破棄時に自動でnullになる。
std::mutex                                    g_reg_mtx;
std::map<obs_source_t*, QPointer<QProgressBar>> g_registry;

// ============================================================
// inject
// ============================================================

struct InjectCtx {
    obs_source_t* source = nullptr;
    int retries_left = kInjectRetryMax;
    explicit InjectCtx(obs_source_t* src)
        : source(src ? obs_source_get_ref(src) : nullptr) {}
    ~InjectCtx() { if (source) obs_source_release(source); }
};

void do_inject(void* param) {
    auto* ctx = static_cast<InjectCtx*>(param);
    if (!ctx) return;

    struct Found { QLabel* label; QString text; };
    std::vector<Found> found;
    std::vector<widget_inject::ScrollSnapshot> scroll_snapshots;

    for (QWidget* w : QApplication::allWidgets()) {
        auto* lbl = qobject_cast<QLabel*>(w);
        if (!lbl) continue;
        const QString text = lbl->text();
        if (text.startsWith(QLatin1String(kMagic)))
            found.push_back({lbl, text});
    }
    for (const auto& f : found)
        widget_inject::collect_ancestor_scroll_snapshot(f.label, scroll_snapshots);

    int replaced = 0;
    for (const auto& f : found) {
        // "FLOWPROG|{value}|{row_label}" をパース
        const QString payload = f.text.mid(static_cast<int>(sizeof(kMagic)) - 1);
        const int sep = payload.indexOf(QLatin1Char('|'));
        int initial_value = 0;
        QString row_label;
        if (sep >= 0) {
            initial_value = payload.left(sep).toInt();
            row_label = payload.mid(sep + 1);
        } else {
            initial_value = payload.toInt();
        }

        QWidget* parent = f.label->parentWidget();
        if (!parent) continue;
        auto* form = qobject_cast<QFormLayout*>(parent->layout());
        if (!form) continue;

        int row = -1;
        QFormLayout::ItemRole role;
        form->getWidgetPosition(f.label, &row, &role);
        if (row < 0) continue;

        auto* bar = new QProgressBar(parent);
        bar->setRange(0, 100);
        bar->setValue(initial_value);
        bar->setTextVisible(true);
        bar->setFormat(QStringLiteral("%p%"));

        form->removeRow(row);
        if (!row_label.isEmpty())
            form->insertRow(row, row_label, bar);
        else
            form->insertRow(row, bar);

        // レジストリに登録
        {
            std::lock_guard<std::mutex> lk(g_reg_mtx);
            g_registry[ctx->source] = QPointer<QProgressBar>(bar);
        }
        ++replaced;
    }

    widget_inject::restore_scroll_snapshots(scroll_snapshots);

    if ((found.empty() || replaced < static_cast<int>(found.size())) &&
        ctx->retries_left > 0) {
        --ctx->retries_left;
        QTimer::singleShot(kInjectRetryMs, [ctx]() { do_inject(ctx); });
        return;
    }
    delete ctx;
}

// ============================================================
// update（プロパティ再構築なしの直接更新）
// ============================================================

struct UpdateCtx {
    obs_source_t* source = nullptr;
    int value = 0;
};

void do_update(void* param) {
    auto* ctx = static_cast<UpdateCtx*>(param);
    if (!ctx) return;

    QPointer<QProgressBar> bar;
    {
        std::lock_guard<std::mutex> lk(g_reg_mtx);
        auto it = g_registry.find(ctx->source);
        if (it != g_registry.end()) bar = it->second;
    }
    if (bar) bar->setValue(ctx->value);

    delete ctx;
}

} // namespace

// ============================================================
// 公開API
// ============================================================

obs_property_t* obs_properties_add_flow_progress(
    obs_properties_t* props,
    const char* prop_name,
    const char* row_label,
    int value)
{
    if (!props || !prop_name || !*prop_name) return nullptr;
    char buf[512];
    snprintf(buf, sizeof(buf), "FLOWPROG|%d|%s", value, row_label ? row_label : "");
    return obs_properties_add_text(props, prop_name, buf, OBS_TEXT_INFO);
}

void schedule_flow_progress_inject(obs_source_t* source)
{
    if (!source) return;
    auto* ctx = new InjectCtx(source);
    obs_queue_task(OBS_TASK_UI, do_inject, ctx, false);
}

void update_flow_progress(obs_source_t* source, int value)
{
    if (!source) return;
    auto* ctx = new UpdateCtx{source, value};
    obs_queue_task(OBS_TASK_UI, do_update, ctx, false);
}

void flow_progress_unregister_source(obs_source_t* source)
{
    if (!source) return;
    std::lock_guard<std::mutex> lk(g_reg_mtx);
    g_registry.erase(source);
}
