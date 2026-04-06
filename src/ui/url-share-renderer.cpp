#include "ui/url-share-renderer.hpp"

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
    out += "出演者は各自、以下のURLをChromeで開いて音声ストリームを再生してください。\r\n"
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
                                        const char* not_configured_text,
                                        const UrlConfirmThemeColors* theme_colors,
                                        bool linkify_urls) {
    const char* not_config = (not_configured_text && *not_configured_text)
                                 ? not_configured_text
                                 : "-";
    const std::string table_bg =
        (theme_colors && !theme_colors->table_bg.empty()) ? theme_colors->table_bg : "#20242b";
    const std::string header_bg =
        (theme_colors && !theme_colors->header_bg.empty()) ? theme_colors->header_bg : "#3a3f4d";
    const std::string header_text =
        (theme_colors && !theme_colors->header_text.empty()) ? theme_colors->header_text : "#ffffff";
    const std::string row_bg =
        (theme_colors && !theme_colors->row_bg.empty()) ? theme_colors->row_bg : "#252a36";
    const std::string alt_row_bg =
        (theme_colors && !theme_colors->alt_row_bg.empty()) ? theme_colors->alt_row_bg : "#2a3040";
    const std::string text =
        (theme_colors && !theme_colors->text.empty()) ? theme_colors->text : "#ffffff";
    const std::string border =
        (theme_colors && !theme_colors->border.empty()) ? theme_colors->border : "#4a5163";
    const std::string link =
        (theme_colors && !theme_colors->link.empty()) ? theme_colors->link : "#74a8ff";

    std::string html;
    html.reserve(3072);
    html += "<table style=\""
            "width:100%; border-collapse:collapse; border-spacing:0;"
            "border:1px solid ";
    html += escape_html_text(border);
    html += "; background-color:";
    html += escape_html_text(table_bg);
    html += "; color:";
    html += escape_html_text(text);
    html += ";\">";
    html += "<tr>"
            "<th style=\"padding:4px 8px; border:1px solid ";
    html += escape_html_text(border);
    html += "; background-color:";
    html += escape_html_text(header_bg);
    html += "; color:";
    html += escape_html_text(header_text);
    html += "; text-align:right;\">Ch.</th>"
            "<th style=\"padding:4px 8px; border:1px solid ";
    html += escape_html_text(border);
    html += "; background-color:";
    html += escape_html_text(header_bg);
    html += "; color:";
    html += escape_html_text(header_text);
    html += "; text-align:left;\">Name</th>"
            "<th style=\"padding:4px 8px; border:1px solid ";
    html += escape_html_text(border);
    html += "; background-color:";
    html += escape_html_text(header_bg);
    html += "; color:";
    html += escape_html_text(header_text);
    html += "; text-align:left;\">URL</th>"
            "</tr>";
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const std::string& bg = (i % 2 == 0) ? row_bg : alt_row_bg;
        html += "<tr style=\"background-color:";
        html += escape_html_text(bg);
        html += ";\"><td style=\"border:1px solid ";
        html += escape_html_text(border);
        html += "; padding:3px 8px; text-align:right;\">";
        html += std::to_string(row.ch_1indexed);
        html += "</td><td style=\"border:1px solid ";
        html += escape_html_text(border);
        html += "; padding:3px 8px;\">";
        if (!row.name.empty()) {
            html += escape_html_text(row.name);
        } else {
            html += "-";
        }
        html += "</td><td style=\"border:1px solid ";
        html += escape_html_text(border);
        html += "; padding:3px 8px;\">";
        if (row.url.empty()) {
            html += escape_html_text(not_config);
        } else {
            std::string escaped_url = escape_html_text(row.url);
            if (linkify_urls) {
                html += "<a style=\"color:";
                html += escape_html_text(link);
                html += ";\" href=\"";
                html += escaped_url;
                html += "\">";
                html += escaped_url;
                html += "</a>";
            } else {
                html += escaped_url;
            }
        }
        html += "</td></tr>";
    }
    html += "</table>";
    return html;
}

} // namespace plugin_main_url_share_renderer
