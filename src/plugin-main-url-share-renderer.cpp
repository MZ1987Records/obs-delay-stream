#include "plugin-main-url-share-renderer.hpp"

namespace plugin_main_url_share_renderer {

namespace {

std::string escape_html_text(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

} // namespace

std::string build_url_share_copy_text(const std::vector<UrlShareRow>& rows,
                                      const char* not_configured_text) {
    const char* not_config = (not_configured_text && *not_configured_text)
                                 ? not_configured_text
                                 : "-";
    std::string out;
    out.reserve(512);
    out += "演者は各自、以下のURLをChromeで開いて音声ストリームを再生してください。\r\n"
           "Each performer should open the following URL in Chrome and play the audio stream.\r\n"
           "\r\n";
    for (const auto& row : rows) {
        out += "- Ch.";
        out += std::to_string(row.ch_1indexed);
        if (!row.name.empty()) {
            out += " ";
            out += row.name;
        }
        out += " ";
        out += row.url.empty() ? not_config : row.url;
        out += "\r\n";
    }
    return out;
}

std::string build_url_confirm_html_text(const std::vector<UrlShareRow>& rows,
                                        const char* not_configured_text) {
    const char* not_config = (not_configured_text && *not_configured_text)
                                 ? not_configured_text
                                 : "-";
    std::string html;
    html.reserve(1536);
    html += "<table style=\"border-collapse:collapse; border:1px solid #999;\">";
    html += "<tr>"
            "<th style=\"border:1px solid #999; padding:2px 6px;\">Ch.</th>"
            "<th style=\"border:1px solid #999; padding:2px 6px;\">Name</th>"
            "<th style=\"border:1px solid #999; padding:2px 6px;\">URL</th>"
            "</tr>";
    for (const auto& row : rows) {
        html += "<tr><td style=\"border:1px solid #999; padding:2px 6px; text-align:right;\">";
        html += std::to_string(row.ch_1indexed);
        html += "</td><td style=\"border:1px solid #999; padding:2px 6px;\">";
        if (!row.name.empty()) {
            html += escape_html_text(row.name);
        } else {
            html += "-";
        }
        html += "</td><td style=\"border:1px solid #999; padding:2px 6px;\">";
        if (row.url.empty()) {
            html += escape_html_text(not_config);
        } else {
            std::string escaped_url = escape_html_text(row.url);
            html += "<a href=\"";
            html += escaped_url;
            html += "\">";
            html += escaped_url;
            html += "</a>";
        }
        html += "</td></tr>";
    }
    html += "</table>";
    return html;
}

} // namespace plugin_main_url_share_renderer
