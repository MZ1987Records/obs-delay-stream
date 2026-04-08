#include "ui/props-refresh.hpp"

#include "plugin/plugin-services.hpp"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QWidget>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ods::ui {

	namespace {

		// 再描画要求 1 件分のキューコンテキスト。
		struct PropsRefreshCtx {
			obs_source_t *source = nullptr;
			uint64_t      seq    = 0;
			std::string   reason;
			~PropsRefreshCtx() {
				if (source) obs_source_release(source);
			}
		};

		// スクロール位置の復元に使うスナップショット。
		struct UiScrollSnapshot {
			QPointer<QScrollBar> bar;
			int                  value = 0;
		};

		// UI 更新停止中に元状態を戻すためのトークン。
		struct UiRefreshFreezeToken {
			QPointer<QWidget> root;
			bool              was_updates_enabled = false;
		};

		constexpr int kPropsRefreshUiFreezeMs = 40;

		// ソース単位の再描画要求をデバウンスし、破棄中ソースを除外して実行する。
		class PropsRefreshController {
		public:

			/// グローバルな再描画キュー制御インスタンスを返す。
			static PropsRefreshController &instance() {
				static PropsRefreshController s;
				return s;
			}

			/// 指定ソースへの再描画要求受付を停止する。
			void block_source(obs_source_t *source) {
				if (!source) return;
				std::lock_guard<std::mutex> lk(pending_mtx_);
				blocked_sources_.insert(source);
				pending_sources_.erase(source);
			}

			/// 指定ソースの再描画要求受付を再開する。
			void unblock_source(obs_source_t *source) {
				if (!source) return;
				std::lock_guard<std::mutex> lk(pending_mtx_);
				blocked_sources_.erase(source);
				pending_sources_.erase(source);
			}

			/// 破棄状態を考慮して安全に再描画要求をキュー投入する。
			void request(obs_source_t *source,
						 bool          create_done,
						 bool          destroying,
						 int           get_props_depth,
						 const char   *reason) {
				if (!source || !create_done) return;
				if (destroying) return;
				if (ods::plugin::is_obs_source_removed(source)) return;
				if (get_props_depth > 0) return;

				uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
				auto     ctx = std::make_unique<PropsRefreshCtx>();
				ctx->seq     = seq;
				ctx->reason  = reason ? reason : "unspecified";
				ctx->source  = obs_source_get_ref(source);
				if (!ctx->source) {
					return;
				}
				{
					std::lock_guard<std::mutex> lk(pending_mtx_);
					// 破棄済み/破棄中ソースへの再描画キュー混入を避ける。
					if (blocked_sources_.find(ctx->source) != blocked_sources_.end()) {
						blog(LOG_INFO, "[obs-delay-stream] props_refresh drop(blocked) seq=%llu reason=%s", (unsigned long long)ctx->seq, ctx->reason.c_str());
						return;
					}
					if (pending_sources_.find(ctx->source) != pending_sources_.end()) {
						blog(LOG_INFO, "[obs-delay-stream] props_refresh drop(pending) seq=%llu reason=%s", (unsigned long long)ctx->seq, ctx->reason.c_str());
						return;
					}
					pending_sources_.insert(ctx->source);
				}

				if (obs_in_task_thread(OBS_TASK_UI)) {
					// UI スレッド起点だと同期待ちになりやすいため graphics を挟んでバウンスする。
					blog(LOG_INFO, "[obs-delay-stream] props_refresh queue(seq=%llu reason=%s via=graphics->ui)", (unsigned long long)ctx->seq, ctx->reason.c_str());
					obs_queue_task(OBS_TASK_GRAPHICS, task_bounce, ctx.release(), false);
				} else {
					blog(LOG_INFO, "[obs-delay-stream] props_refresh queue(seq=%llu reason=%s via=ui)", (unsigned long long)ctx->seq, ctx->reason.c_str());
					obs_queue_task(OBS_TASK_UI, task_refresh_ui, ctx.release(), false);
				}
			}

		private:

			PropsRefreshController() = default;

			/// graphics スレッドを経由して UI スレッドへ再投入する。
			static void task_bounce(void *p) {
				obs_queue_task(OBS_TASK_UI, task_refresh_ui, p, false);
			}

			/// 1件分のプロパティ再描画要求を UI スレッドで実行する。
			static void task_refresh_ui(void *p) {
				auto ctx = std::unique_ptr<PropsRefreshCtx>(static_cast<PropsRefreshCtx *>(p));
				if (!ctx || !ctx->source) {
					return;
				}
				bool  should_update = true;
				auto &controller    = instance();
				{
					std::lock_guard<std::mutex> lk(controller.pending_mtx_);
					controller.pending_sources_.erase(ctx->source);
					if (controller.blocked_sources_.find(ctx->source) !=
						controller.blocked_sources_.end()) {
						should_update = false;
					}
				}
				if (should_update && ods::plugin::is_obs_source_removed(ctx->source)) {
					should_update = false;
				}
				if (should_update) {
					blog(LOG_INFO, "[obs-delay-stream] props_refresh exec seq=%llu reason=%s", (unsigned long long)ctx->seq, ctx->reason.c_str());
					props_ui_with_preserved_scroll([&]() {
						obs_source_update_properties(ctx->source);
					});
				} else {
					blog(LOG_INFO, "[obs-delay-stream] props_refresh skip seq=%llu reason=%s", (unsigned long long)ctx->seq, ctx->reason.c_str());
				}
			}

			std::mutex               pending_mtx_;
			std::set<obs_source_t *> pending_sources_;
			std::set<obs_source_t *> blocked_sources_;
			std::atomic<uint64_t>    seq_{0};
		};

		/// 現在のプロパティダイアログのルートウィジェットを推定する。
		QWidget *find_properties_root_widget() {
			QWidget *root = QApplication::activeModalWidget();
			if (!root) root = QApplication::activeWindow();
			if (!root) {
				QWidget *focus = QApplication::focusWidget();
				if (focus) root = focus->window();
			}
			return root;
		}

		/// 再描画中のちらつき防止のため、UI更新を一時停止する。
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

		/// プロパティルート配下の再描画を即時要求する。
		void request_properties_root_repaint(QWidget *root) {
			if (!root) return;
			root->update();
			root->repaint();

			auto repaint_area = [](QAbstractScrollArea *area) {
				if (!area) return;
				area->update();
				if (QWidget *vp = area->viewport()) {
					vp->update();
					vp->repaint();
				}
			};

			repaint_area(qobject_cast<QAbstractScrollArea *>(root));
			const auto areas = root->findChildren<QAbstractScrollArea *>();
			for (QAbstractScrollArea *area : areas) {
				repaint_area(area);
			}
		}

		/// freeze 前の状態へ UI 更新可否を戻し、再描画を促す。
		void unfreeze_properties_ui_updates(const UiRefreshFreezeToken &token) {
			if (!token.root || !token.was_updates_enabled) return;
			QPointer<QWidget> root = token.root;
			root->setUpdatesEnabled(true);
			request_properties_root_repaint(root.data());
			QTimer::singleShot(0, root.data(), [root]() {
				request_properties_root_repaint(root.data());
			});
		}

		/// 表示中スクロールバーの位置をスナップショットとして保存する。
		std::vector<UiScrollSnapshot> snapshot_vertical_scrollbars() {
			std::vector<UiScrollSnapshot> snapshots;
			QWidget                      *root = find_properties_root_widget();
			if (!root) return snapshots;

			std::set<QScrollBar *> unique_bars;
			auto                   add_snapshot = [&](QAbstractScrollArea *area) {
				if (!area) return;
				QScrollBar *bar = area->verticalScrollBar();
				if (!bar) return;
				if (!unique_bars.insert(bar).second) return;
				snapshots.push_back({QPointer<QScrollBar>(bar), bar->value()});
			};

			add_snapshot(qobject_cast<QAbstractScrollArea *>(root));
			const auto areas = root->findChildren<QAbstractScrollArea *>();
			snapshots.reserve(areas.size() + 1);
			for (QAbstractScrollArea *area : areas) {
				add_snapshot(area);
			}
			return snapshots;
		}

		/// 保存済みスクロール位置を可能な範囲で復元する。
		void restore_vertical_scrollbars(const std::vector<UiScrollSnapshot> &snapshots) {
			for (const auto &snap : snapshots) {
				if (!snap.bar) continue;
				snap.bar->setValue(snap.value);
			}
		}

	} // namespace

	void props_ui_with_preserved_scroll(const std::function<void()> &body) {
		const auto scroll_snapshots = snapshot_vertical_scrollbars();
		const auto freeze_token     = freeze_properties_ui_updates();
		if (body) body();
		restore_vertical_scrollbars(scroll_snapshots);
		QTimer::singleShot(0, qApp, [scroll_snapshots]() {
			restore_vertical_scrollbars(scroll_snapshots);
		});
		// ウィジェット再レイアウト後にも復元を再適用して飛びを抑える。
		QTimer::singleShot(kPropsRefreshUiFreezeMs, qApp, [scroll_snapshots, freeze_token]() {
			restore_vertical_scrollbars(scroll_snapshots);
			unfreeze_properties_ui_updates(freeze_token);
		});
	}

	void props_refresh_unblock_source(obs_source_t *source) {
		PropsRefreshController::instance().unblock_source(source);
	}

	void props_refresh_block_source(obs_source_t *source) {
		PropsRefreshController::instance().block_source(source);
	}

	void props_refresh_request(obs_source_t *source,
							   bool          create_done,
							   bool          destroying,
							   int           get_props_depth,
							   const char   *reason) {
		PropsRefreshController::instance().request(
			source,
			create_done,
			destroying,
			get_props_depth,
			reason);
	}

} // namespace ods::ui
