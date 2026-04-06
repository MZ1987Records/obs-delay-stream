#pragma once

#include <string>
#include <vector>

namespace plugin_main_url_share_renderer {

struct UrlShareRow {
	int         ch_1indexed = 0;
	std::string name;
	std::string url;
};

struct UrlConfirmThemeColors {
	std::string table_bg;
	std::string header_bg;
	std::string header_text;
	std::string row_bg;
	std::string alt_row_bg;
	std::string text;
	std::string border;
	std::string link;
};

std::string build_url_share_copy_text(const std::vector<UrlShareRow> &rows,
									  const char                     *not_configured_text);
std::string build_url_confirm_html_text(const std::vector<UrlShareRow> &rows,
										const char                     *not_configured_text,
										const UrlConfirmThemeColors    *theme_colors = nullptr,
										bool                            linkify_urls = true);

} // namespace plugin_main_url_share_renderer
