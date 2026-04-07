#pragma once

#include <functional>
#include <obs-module.h>

namespace ods::ui {

	/// 指定ソースに対するプロパティ再構築ブロックを解除する
	void props_refresh_unblock_source(obs_source_t *source);

	/// 指定ソースに対するプロパティ再構築を一時的にブロックする
	void props_refresh_block_source(obs_source_t *source);

	/// 条件を確認しつつプロパティ再描画を要求する
	void props_refresh_request(obs_source_t *source,
							   bool          create_done,
							   bool          destroying,
							   int           get_props_depth,
							   const char   *reason);

	/// OBS プロパティ再構築時のちらつき抑制とスクロール位置維持を共通化する
	void props_ui_with_preserved_scroll(const std::function<void()> &body);

} // namespace ods::ui
