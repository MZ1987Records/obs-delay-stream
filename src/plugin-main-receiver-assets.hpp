#pragma once

#include <string>

namespace plugin_main_receiver_assets {

std::string load_receiver_index_html();
std::string get_receiver_root_dir();
const char* get_receiver_build_timestamp();

} // namespace plugin_main_receiver_assets
