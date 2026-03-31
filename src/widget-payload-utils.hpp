#pragma once
#ifndef OBS_WIDGET_PAYLOAD_UTILS_HPP_
#define OBS_WIDGET_PAYLOAD_UTILS_HPP_

#include <QString>
#include <QStringList>

#include <string>

namespace widget_payload {

inline std::string escape_field(const char *src)
{
    std::string out;
    if (!src)
        return out;
    while (*src) {
        const char c = *src++;
        if (c == '\\' || c == '|')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

inline bool split_escaped_pipe_fields(const QString &text, QStringList &fields)
{
    fields.clear();
    QString cur;
    bool escaped = false;
    for (QChar ch : text) {
        if (escaped) {
            cur.append(ch);
            escaped = false;
            continue;
        }
        if (ch == QChar('\\')) {
            escaped = true;
            continue;
        }
        if (ch == QChar('|')) {
            fields.push_back(cur);
            cur.clear();
            continue;
        }
        cur.append(ch);
    }
    if (escaped)
        cur.append(QChar('\\'));
    fields.push_back(cur);
    return true;
}

} // namespace widget_payload

#endif // OBS_WIDGET_PAYLOAD_UTILS_HPP_
