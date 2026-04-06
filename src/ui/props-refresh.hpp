#pragma once

#include <functional>

#include <obs-module.h>

namespace plugin_main_props_refresh {

void props_refresh_unblock_source(obs_source_t* source);
void props_refresh_block_source(obs_source_t* source);
void props_refresh_request(obs_source_t* source,
                           bool create_done,
                           bool destroying,
                           int get_props_depth,
                           const char* reason);

// OBS プロパティ再構築時のちらつき抑制とスクロール位置維持を共通化する。
void props_ui_with_preserved_scroll(const std::function<void()>& body);

} // namespace plugin_main_props_refresh
