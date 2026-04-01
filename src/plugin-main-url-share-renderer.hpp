#pragma once

#include <string>
#include <vector>

namespace plugin_main_url_share_renderer {

struct UrlShareRow {
    int ch_1indexed = 0;
    std::string name;
    std::string url;
};

std::string build_url_share_copy_text(const std::vector<UrlShareRow>& rows,
                                      const char* not_configured_text);
std::string build_url_confirm_html_text(const std::vector<UrlShareRow>& rows,
                                        const char* not_configured_text);

} // namespace plugin_main_url_share_renderer
