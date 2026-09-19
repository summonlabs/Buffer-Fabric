#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

/// Minimal deterministic JSON writer used by the CLI and explanation output.
/// Output is bounded: every append is checked against a byte budget and the
/// writer latches into a truncated state rather than growing without limit.
class BF_API JsonWriter {
public:
    explicit JsonWriter(usize byte_budget = 1u << 20) : budget_(byte_budget) {}

    void begin_object();
    void end_object();
    void begin_array();
    void end_array();
    void key(std::string_view name);
    void value_string(std::string_view text);
    void value_u64(u64 value);
    void value_i64(i64 value);
    void value_bool(bool value);
    void value_null();
    void raw(std::string_view text);

    [[nodiscard]] const std::string& str() const noexcept { return out_; }
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] std::string take() { return std::move(out_); }

private:
    void comma();
    void push(std::string_view text);

    std::string out_{};
    usize budget_{};
    bool truncated_{false};
    bool need_comma_{false};
    std::vector<bool> in_object_{};
};

}  // namespace buffer_fabric
