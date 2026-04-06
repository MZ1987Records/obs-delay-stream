#pragma once

#include <string>

namespace ods::plugin {

std::string load_receiver_index_html();
std::string get_receiver_root_dir();
const char *get_receiver_build_timestamp();

} // namespace ods::plugin
