#include "ui/props-refresh.hpp"

#include <set>
#include <vector>
#include <mutex>
#include <atomic>

#include <QApplication>
#include <QAbstractScrollArea>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QWidget>

#include "plugin/plugin-services.hpp"

namespace plugin_main_props_refresh {

namespace {

struct PropsRefreshCtx {
    obs_source_t* source = nullptr;
    uint64_t      seq = 0;
    const char*   reason = nullptr;
};

struct UiScrollSnapshot {
    QPointer<QScrollBar> bar;
    int value = 0;
};

struct UiRefreshFreezeToken {
    QPointer<QWidget> root;
    bool was_updates_enabled = false;
};

constexpr int kPropsRefreshUiFreezeMs = 40;

std::mutex              g_props_refresh_pending_mtx;
std::set<obs_source_t*> g_props_refresh_pending_sources;
std::set<obs_source_t*> g_props_refresh_blocked_sources;
std::atomic<uint64_t>   g_props_refresh_seq{0};

QWidget* find_properties_root_widget() {
    QWidget* root = QApplication::activeModalWidget();
    if (!root) root = QApplication::activeWindow();
    if (!root) {
        QWidget* focus = QApplication::focusWidget();
        if (focus) root = focus->window();
    }
    return root;
}

UiRefreshFreezeToken freeze_properties_ui_updates() {
    UiRefreshFreezeToken token;
    token.root = find_properties_root_widget();
    if (!token.root) return token;
    token.was_updates_enabled = token.root->updatesEnabled();
    if (token.was_updates_enabled) {
        token.root->setUpdatesEnabled(false);
    }
    return token;
}

void request_properties_root_repaint(QWidget* root) {
    if (!root) return;
    root->update();
    root->repaint();

    auto repaint_area = [](QAbstractScrollArea* area) {
        if (!area) return;
        area->update();
        if (QWidget* vp = area->viewport()) {
            vp->update();
            vp->repaint();
        }
    };

    repaint_area(qobject_cast<QAbstractScrollArea*>(root));
    const auto areas = root->findChildren<QAbstractScrollArea*>();
    for (QAbstractScrollArea* area : areas) {
        repaint_area(area);
    }
}

void unfreeze_properties_ui_updates(const UiRefreshFreezeToken& token) {
    if (!token.root || !token.was_updates_enabled) return;
    QPointer<QWidget> root = token.root;
    root->setUpdatesEnabled(true);
    request_properties_root_repaint(root.data());
    QTimer::singleShot(0, root.data(), [root]() {
        request_properties_root_repaint(root.data());
    });
}

std::vector<UiScrollSnapshot> snapshot_vertical_scrollbars() {
    std::vector<UiScrollSnapshot> snapshots;
    QWidget* root = find_properties_root_widget();
    if (!root) return snapshots;

    std::set<QScrollBar*> unique_bars;
    auto add_snapshot = [&](QAbstractScrollArea* area) {
        if (!area) return;
        QScrollBar* bar = area->verticalScrollBar();
        if (!bar) return;
        if (!unique_bars.insert(bar).second) return;
        snapshots.push_back({QPointer<QScrollBar>(bar), bar->value()});
    };

    add_snapshot(qobject_cast<QAbstractScrollArea*>(root));
    const auto areas = root->findChildren<QAbstractScrollArea*>();
    snapshots.reserve(areas.size() + 1);
    for (QAbstractScrollArea* area : areas) {
        add_snapshot(area);
    }
    return snapshots;
}

void restore_vertical_scrollbars(const std::vector<UiScrollSnapshot>& snapshots) {
    for (const auto& snap : snapshots) {
        if (!snap.bar) continue;
        snap.bar->setValue(snap.value);
    }
}

// UIスレッド側でプロパティ再構築を実行する。
void do_request_properties_refresh_ui(void* p) {
    auto* ctx = static_cast<PropsRefreshCtx*>(p);
    if (!ctx || !ctx->source) {
        delete ctx;
        return;
    }
    bool should_update = true;
    {
        std::lock_guard<std::mutex> lk(g_props_refresh_pending_mtx);
        g_props_refresh_pending_sources.erase(ctx->source);
        if (g_props_refresh_blocked_sources.find(ctx->source) !=
            g_props_refresh_blocked_sources.end()) {
            should_update = false;
        }
    }
    if (should_update && plugin_main_obs_services::is_obs_source_removed(ctx->source)) {
        should_update = false;
    }
    if (should_update) {
        blog(LOG_INFO, "[obs-delay-stream] props_refresh exec seq=%llu reason=%s",
             (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
        props_ui_with_preserved_scroll([&]() {
            obs_source_update_properties(ctx->source);
        });
    } else {
        blog(LOG_INFO, "[obs-delay-stream] props_refresh skip seq=%llu reason=%s",
             (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
    }
    obs_source_release(ctx->source);
    delete ctx;
}

// UIスレッド再入を避けるため、いったん別スレッドへ退避してからUIへ戻す。
void bounce_request_properties_refresh(void* p) {
    obs_queue_task(OBS_TASK_UI, do_request_properties_refresh_ui, p, false);
}

} // namespace

void props_ui_with_preserved_scroll(const std::function<void()>& body) {
    const auto scroll_snapshots = snapshot_vertical_scrollbars();
    const auto freeze_token = freeze_properties_ui_updates();
    if (body) body();
    restore_vertical_scrollbars(scroll_snapshots);
    QTimer::singleShot(0, qApp, [scroll_snapshots]() {
        restore_vertical_scrollbars(scroll_snapshots);
    });
    QTimer::singleShot(kPropsRefreshUiFreezeMs, qApp, [scroll_snapshots, freeze_token]() {
        restore_vertical_scrollbars(scroll_snapshots);
        unfreeze_properties_ui_updates(freeze_token);
    });
}

void props_refresh_unblock_source(obs_source_t* source) {
    if (!source) return;
    std::lock_guard<std::mutex> lk(g_props_refresh_pending_mtx);
    g_props_refresh_blocked_sources.erase(source);
    g_props_refresh_pending_sources.erase(source);
}

void props_refresh_block_source(obs_source_t* source) {
    if (!source) return;
    std::lock_guard<std::mutex> lk(g_props_refresh_pending_mtx);
    g_props_refresh_blocked_sources.insert(source);
    g_props_refresh_pending_sources.erase(source);
}

void props_refresh_request(obs_source_t* source,
                           bool create_done,
                           bool destroying,
                           int get_props_depth,
                           const char* reason) {
    if (!source || !create_done) return;
    if (destroying) return;
    if (plugin_main_obs_services::is_obs_source_removed(source)) return;
    if (get_props_depth > 0) return;

    uint64_t seq = g_props_refresh_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    auto* ctx = new PropsRefreshCtx();
    ctx->seq = seq;
    ctx->reason = reason ? reason : "unspecified";
    ctx->source = obs_source_get_ref(source);
    if (!ctx->source) {
        delete ctx;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_props_refresh_pending_mtx);
        if (g_props_refresh_blocked_sources.find(ctx->source) !=
            g_props_refresh_blocked_sources.end()) {
            blog(LOG_INFO, "[obs-delay-stream] props_refresh drop(blocked) seq=%llu reason=%s",
                 (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
            obs_source_release(ctx->source);
            delete ctx;
            return;
        }
        if (g_props_refresh_pending_sources.find(ctx->source) !=
            g_props_refresh_pending_sources.end()) {
            blog(LOG_INFO, "[obs-delay-stream] props_refresh drop(pending) seq=%llu reason=%s",
                 (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
            obs_source_release(ctx->source);
            delete ctx;
            return;
        }
        g_props_refresh_pending_sources.insert(ctx->source);
    }

    if (obs_in_task_thread(OBS_TASK_UI)) {
        blog(LOG_INFO, "[obs-delay-stream] props_refresh queue(seq=%llu reason=%s via=graphics->ui)",
             (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
        obs_queue_task(OBS_TASK_GRAPHICS, bounce_request_properties_refresh, ctx, false);
    } else {
        blog(LOG_INFO, "[obs-delay-stream] props_refresh queue(seq=%llu reason=%s via=ui)",
             (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
        obs_queue_task(OBS_TASK_UI, do_request_properties_refresh_ui, ctx, false);
    }
}

} // namespace plugin_main_props_refresh
