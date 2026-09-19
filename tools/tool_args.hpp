#pragma once

// Shared command-line parsing helpers for the Buffer Fabric tools.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bftool {

struct Arguments {
    std::vector<std::string> tokens{};

    [[nodiscard]] bool has(const std::string& flag) const {
        for (const auto& token : tokens) {
            if (token == flag) return true;
        }
        return false;
    }

    [[nodiscard]] std::string value(const std::string& flag, const std::string& fallback = {}) const {
        for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
            if (tokens[index] == flag) return tokens[index + 1];
        }
        return fallback;
    }

    [[nodiscard]] unsigned long long number(const std::string& flag, unsigned long long fallback) const {
        const std::string text = value(flag);
        if (text.empty()) return fallback;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
        if (end == text.c_str()) return fallback;
        return parsed;
    }
};

/// True when \p flag is a long option that consumes the following token.
inline bool flag_takes_value(const std::string& flag) {
    static const char* const kFlags[] = {
        "--state-dir",    "--capacity",       "--pool-units", "--protected", "--queue-id",
        "--demand",       "--pool",           "--queue",      "--units",     "--attempt",
        "--allocation",   "--ttl",            "--address",    "--port",      "--run-seconds",
        "--resource-units", "--ops",          "--pools",      "--queues",    "--seed",
        "--host",         "--claim-epoch",    "--expected-epoch", "--label", "--filter",
        "--command",
    };
    for (const char* candidate : kFlags) {
        if (flag == candidate) return true;
    }
    return false;
}

/// The first token that is neither a flag nor the value of a flag.
inline std::string first_positional(const Arguments& arguments) {
    bool expect_value = false;
    for (const auto& token : arguments.tokens) {
        if (expect_value) {
            expect_value = false;
            continue;
        }
        if (token.size() >= 2 && token[0] == '-' && token[1] == '-') {
            expect_value = flag_takes_value(token);
            continue;
        }
        return token;
    }
    return std::string{};
}

inline Arguments parse(int argc, char** argv) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.tokens.emplace_back(argv[index]);
    }
    return arguments;
}

inline void print_usage(const char* program, const char* usage) {
    std::printf("usage: %s %s\n", program, usage);
}

}  // namespace bftool
