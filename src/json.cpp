#include "buffer_fabric/json.hpp"

#include <cstdio>

namespace buffer_fabric {
namespace {

void append_escaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out.append(buffer);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}

}  // namespace

void JsonWriter::push(std::string_view text) {
    if (truncated_) return;
    if (out_.size() + text.size() > budget_) {
        truncated_ = true;
        static const std::string kMarker = "\"__truncated__\":true";
        if (out_.size() + kMarker.size() <= budget_) {
            comma();
            out_.append(kMarker);
        }
        return;
    }
    out_.append(text);
}

void JsonWriter::comma() {
    // A separator is an append like any other and must obey the byte budget,
    // otherwise an object with many members could grow without bound.
    if (need_comma_) push(",");
    need_comma_ = false;
}

void JsonWriter::begin_object() {
    comma();
    push("{");
    in_object_.push_back(true);
    need_comma_ = false;
}

void JsonWriter::end_object() {
    push("}");
    if (!in_object_.empty()) in_object_.pop_back();
    need_comma_ = true;
}

void JsonWriter::begin_array() {
    comma();
    push("[");
    in_object_.push_back(false);
    need_comma_ = false;
}

void JsonWriter::end_array() {
    push("]");
    if (!in_object_.empty()) in_object_.pop_back();
    need_comma_ = true;
}

void JsonWriter::key(std::string_view name) {
    comma();
    std::string escaped;
    append_escaped(escaped, name);
    push(escaped);
    push(":");
}

void JsonWriter::value_string(std::string_view text) {
    comma();
    std::string escaped;
    append_escaped(escaped, text);
    push(escaped);
    need_comma_ = true;
}

void JsonWriter::value_u64(u64 value) {
    comma();
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    push(buffer);
    need_comma_ = true;
}

void JsonWriter::value_i64(i64 value) {
    comma();
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    push(buffer);
    need_comma_ = true;
}

void JsonWriter::value_bool(bool value) {
    comma();
    push(value ? "true" : "false");
    need_comma_ = true;
}

void JsonWriter::value_null() {
    comma();
    push("null");
    need_comma_ = true;
}

void JsonWriter::raw(std::string_view text) {
    comma();
    push(text);
    need_comma_ = true;
}

}  // namespace buffer_fabric
