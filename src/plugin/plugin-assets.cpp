#include "plugin/plugin-assets.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include "plugin/plugin-utils.hpp"
#include "receiver_index_html.hpp"

namespace plugin_main_receiver_assets {

namespace {

constexpr const char* kReceiverBuildTimestamp = __DATE__ " " __TIME__;

}

const char* get_receiver_build_timestamp() {
    return kReceiverBuildTimestamp;
}

std::string load_receiver_index_html() {
    std::string html;
    char* mod_path = obs_module_file("receiver/index.html");
    if (mod_path) {
        html = plugin_utils::read_file_to_string(mod_path);
        bfree(mod_path);
    }
    if (html.empty()) {
        html = plugin_utils::read_file_to_string("receiver/index.html");
    }
    if (html.empty()) {
        html = std::string(kReceiverIndexHtml);
    }
    plugin_utils::replace_all(html, "@PROJECT_VERSION@", PLUGIN_VERSION);
    plugin_utils::replace_all(html, "@BUILD_TIMESTAMP@", kReceiverBuildTimestamp);
    return html;
}

std::string get_receiver_root_dir() {
    std::string path;
    char* mod_path = obs_module_file("receiver/index.html");
    if (mod_path) {
        path = mod_path;
        bfree(mod_path);
    }
    if (path.empty()) {
        path = "receiver/index.html";
    }
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    return path.substr(0, pos);
}

} // namespace plugin_main_receiver_assets
