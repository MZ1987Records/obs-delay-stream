#include "widgets/flow-progress-widget.hpp"

#include "widgets/widget-inject-utils.hpp"

#include <QApplication>
#include <QFormLayout>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QSizePolicy>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ods::widgets {

	namespace {

		// プレースホルダーQLabel に埋め込むマジックプレフィックス。
		// フォーマット: "FLOWPROG|{value}|{row_label}"
		constexpr char kMagic[]        = "FLOWPROG|";
		constexpr int  kInjectRetryMax = 40;
		constexpr int  kInjectRetryMs  = 5;

		// ソース → QProgressBar の弱参照レジストリ。
		// inject 後に登録し、update_flow_progress で直接 setValue() するために使う。
		// QPointer はウィジェット破棄時に自動でnullになる。
		std::mutex                                       g_reg_mtx;
		std::map<obs_source_t *, QPointer<QProgressBar>> g_registry;

		// ============================================================
		// inject
		// ============================================================

		// inject 再試行状態を保持する UI タスク用コンテキスト。
		struct InjectCtx {
			obs_source_t *source       = nullptr;
			int           retries_left = kInjectRetryMax;
			explicit InjectCtx(obs_source_t *src)
				: source(src ? obs_source_get_ref(src) : nullptr) {}
			~InjectCtx() {
				if (source) obs_source_release(source);
			}
		};

		/// プレースホルダーを QProgressBar へ差し替える inject 本体。
		void do_inject(void *param) {
			auto ctx = std::unique_ptr<InjectCtx>(static_cast<InjectCtx *>(param));
			if (!ctx) return;

			struct Found {
				QLabel *label;
				QString text;
			};
			std::vector<Found>          found;
			std::vector<ScrollSnapshot> scroll_snapshots;

			for (QWidget *w : QApplication::allWidgets()) {
				auto *lbl = qobject_cast<QLabel *>(w);
				if (!lbl) continue;
				const QString text = lbl->text();
				if (text.startsWith(QLatin1String(kMagic)))
					found.push_back({lbl, text});
			}
			for (const auto &f : found)
				collect_ancestor_scroll_snapshot(f.label, scroll_snapshots);

			int replaced = 0;
			for (const auto &f : found) {
				// "FLOWPROG|{value}|{row_label}|{bar_text}" をパース
				const QString payload       = f.text.mid(static_cast<int>(sizeof(kMagic)) - 1);
				const int     sep1          = payload.indexOf(QLatin1Char('|'));
				int           initial_value = 0;
				QString       row_label;
				QString       bar_text;
				if (sep1 >= 0) {
					initial_value      = payload.left(sep1).toInt();
					const QString rest = payload.mid(sep1 + 1);
					const int     sep2 = rest.indexOf(QLatin1Char('|'));
					if (sep2 >= 0) {
						row_label = rest.left(sep2);
						bar_text  = rest.mid(sep2 + 1);
					} else {
						row_label = rest;
					}
				} else {
					initial_value = payload.toInt();
				}

				QWidget *parent = f.label->parentWidget();
				if (!parent) continue;
				auto *form = qobject_cast<QFormLayout *>(parent->layout());
				if (!form) continue;

				int                   row = -1;
				QFormLayout::ItemRole role;
				form->getWidgetPosition(f.label, &row, &role);
				if (row < 0) continue;
				// bar_text の長短に関わらずバー高さを固定するため sizeHint() は使わない
				const QMargins text_margins = f.label->contentsMargins();
				constexpr int  kBarHeight   = 14;
				constexpr int  kBarMarginV  = 2;

				auto *bar = new QProgressBar(parent);
				bar->setRange(0, 100);
				bar->setValue(initial_value);
				bar->setTextVisible(true);
				if (!bar_text.isEmpty()) {
					// % は QProgressBar フォーマット文字のためリテラル % を %% に変換する
					QString fmt = bar_text;
					fmt.replace(QLatin1Char('%'), QStringLiteral("%%"));
					bar->setFormat(fmt);
				} else {
					bar->setFormat(QStringLiteral("%p%"));
				}
				bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
				bar->setFixedHeight(kBarHeight);
				bar->setStyleSheet(QStringLiteral(
					"QProgressBar {"
					" border:1px solid palette(mid);"
					" border-radius:3px;"
					" background-color:palette(base);"
					" text-align:center;"
					" padding:0px;"
					"}"
					"QProgressBar::chunk {"
					" background-color:palette(highlight);"
					" border:none;"
					" border-radius:0px;"
					"}"));
				auto *bar_host = new QWidget(parent);
				bar_host->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
				bar_host->setFixedHeight(kBarHeight + kBarMarginV * 2);
				auto *bar_lay = new QVBoxLayout(bar_host);
				bar_lay->setContentsMargins(
					text_margins.left(),
					kBarMarginV,
					text_margins.right(),
					kBarMarginV);
				bar_lay->setSpacing(0);
				bar_lay->addWidget(bar);

				form->removeRow(row);
				if (!row_label.isEmpty())
					form->insertRow(row, row_label, bar_host);
				else
					form->insertRow(row, bar_host);

				// レジストリに登録
				{
					std::lock_guard<std::mutex> lk(g_reg_mtx);
					g_registry[ctx->source] = QPointer<QProgressBar>(bar);
				}
				++replaced;
			}

			restore_scroll_snapshots(scroll_snapshots);

			if ((found.empty() || replaced < static_cast<int>(found.size())) &&
				ctx->retries_left > 0) {
				--ctx->retries_left;
				auto *next = ctx.release();
				QTimer::singleShot(kInjectRetryMs, [next]() { do_inject(next); });
				return;
			}
		}

		// ============================================================
		// update（プロパティ再構築なしの直接更新）
		// ============================================================

		// 値更新を UI スレッドへ渡すためのコンテキスト。
		struct UpdateCtx {
			obs_source_t *source = nullptr;
			int           value  = 0;
		};

		/// レジストリ経由で対象プログレスバー値を更新する。
		void do_update(void *param) {
			auto ctx = std::unique_ptr<UpdateCtx>(static_cast<UpdateCtx *>(param));
			if (!ctx) return;

			QPointer<QProgressBar> bar;
			{
				std::lock_guard<std::mutex> lk(g_reg_mtx);
				auto                        it = g_registry.find(ctx->source);
				if (it != g_registry.end()) bar = it->second;
			}
			if (bar) bar->setValue(ctx->value);
		}

	} // namespace

	// ============================================================
	// 公開API
	// ============================================================

	obs_property_t *obs_properties_add_flow_progress(
		obs_properties_t *props,
		const char       *prop_name,
		const char       *row_label,
		int               value,
		const char       *bar_text) {
		if (!props || !prop_name || !*prop_name) return nullptr;
		const std::string payload = "FLOWPROG|" + std::to_string(value) + "|" +
									(row_label ? row_label : "") + "|" +
									(bar_text ? bar_text : "");
		return obs_properties_add_text(props, prop_name, payload.c_str(), OBS_TEXT_INFO);
	}

	void schedule_flow_progress_inject(obs_source_t *source) {
		if (!source) return;
		auto ctx = std::make_unique<InjectCtx>(source);
		obs_queue_task(OBS_TASK_UI, do_inject, ctx.release(), false);
	}

	void update_flow_progress(obs_source_t *source, int value) {
		if (!source) return;
		auto ctx = std::make_unique<UpdateCtx>(UpdateCtx{source, value});
		obs_queue_task(OBS_TASK_UI, do_update, ctx.release(), false);
	}

	void flow_progress_unregister_source(obs_source_t *source) {
		if (!source) return;
		std::lock_guard<std::mutex> lk(g_reg_mtx);
		g_registry.erase(source);
	}

} // namespace ods::widgets
