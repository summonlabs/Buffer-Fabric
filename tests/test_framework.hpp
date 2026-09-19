#pragma once

// Minimal test framework for Buffer Fabric.
//
// Deliberately dependency-free so that a fresh clone builds and tests with no
// network access and no package manager. Tests are registered statically and
// run sequentially by name order; a failure aborts the current test only.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace bftest {

struct Failure {
    std::string message;
};

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> body) {
        registry().push_back(TestCase{suite, name, std::move(body)});
    }
};

inline void fail(const char* file, int line, const std::string& expression,
                 const std::string& detail) {
    std::ostringstream stream;
    stream << file << ":" << line << ": check failed: " << expression;
    if (!detail.empty()) stream << " (" << detail << ")";
    throw Failure{stream.str()};
}

template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>()
                                             << std::declval<const T&>())>> : std::true_type {};

template <class T>
inline std::string describe(const T& value) {
    if constexpr (is_streamable<T>::value) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    } else if constexpr (std::is_enum_v<T>) {
        std::ostringstream stream;
        stream << static_cast<long long>(value);
        return stream.str();
    } else {
        return std::string("<unprintable>");
    }
}

inline std::string describe(bool value) { return value ? "true" : "false"; }

inline std::string describe(const std::string& value) { return "\"" + value + "\""; }

inline int run_all(int argc, char** argv) {
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--filter=", 0) == 0) {
            filter = argument.substr(9);
        } else if (argument == "--list") {
            for (const auto& test : registry()) {
                std::cout << test.suite << "." << test.name << "\n";
            }
            return 0;
        }
    }
    std::vector<TestCase> selected;
    for (const auto& test : registry()) {
        const std::string full = test.suite + "." + test.name;
        if (filter.empty() || full.find(filter) != std::string::npos) selected.push_back(test);
    }
    std::sort(selected.begin(), selected.end(), [](const TestCase& a, const TestCase& b) {
        if (a.suite != b.suite) return a.suite < b.suite;
        return a.name < b.name;
    });

    std::size_t passed = 0;
    std::vector<std::string> failures;
    for (const auto& test : selected) {
        const std::string full = test.suite + "." + test.name;
        try {
            test.body();
            ++passed;
            std::cout << "[  ok  ] " << full << "\n";
        } catch (const Failure& failure) {
            failures.push_back(full + ": " + failure.message);
            std::cout << "[ FAIL ] " << full << "\n         " << failure.message << "\n";
        } catch (const std::exception& error) {
            failures.push_back(full + ": unexpected exception: " + error.what());
            std::cout << "[ FAIL ] " << full << "\n         unexpected exception: " << error.what()
                      << "\n";
        } catch (...) {
            failures.push_back(full + ": unknown exception");
            std::cout << "[ FAIL ] " << full << "\n         unknown exception\n";
        }
        std::cout.flush();
    }
    std::cout << "\n" << passed << " passed, " << failures.size() << " failed, " << selected.size()
              << " total\n";
    if (!failures.empty()) {
        std::cout << "\nfailures:\n";
        for (const auto& failure : failures) {
            std::cout << "  - " << failure << "\n";
        }
        return 1;
    }
    return 0;
}

}  // namespace bftest

#define BF_TEST(suite_name, test_name)                                                     \
    static void bf_test_body_##suite_name##_##test_name();                                 \
    static ::bftest::Registrar bf_test_reg_##suite_name##_##test_name(                     \
        #suite_name, #test_name, bf_test_body_##suite_name##_##test_name);                 \
    static void bf_test_body_##suite_name##_##test_name()

#define BF_CHECK(expr)                                                        \
    do {                                                                      \
        if (!(expr)) {                                                        \
            ::bftest::fail(__FILE__, __LINE__, #expr, std::string());          \
        }                                                                     \
    } while (false)

#define BF_REQUIRE(expr)                                                      \
    do {                                                                      \
        if (!(expr)) {                                                        \
            ::bftest::fail(__FILE__, __LINE__, #expr, std::string());          \
        }                                                                     \
    } while (false)

#define BF_CHECK_EQ(actual, expected)                                                     \
    do {                                                                                  \
        const auto& bf_actual_ = (actual);                                                \
        const auto& bf_expected_ = (expected);                                            \
        if (!(bf_actual_ == bf_expected_)) {                                              \
            ::bftest::fail(__FILE__, __LINE__, #actual " == " #expected,                  \
                           "actual=" + ::bftest::describe(bf_actual_) +                   \
                               " expected=" + ::bftest::describe(bf_expected_));          \
        }                                                                                 \
    } while (false)

#define BF_CHECK_NE(actual, unexpected)                                                   \
    do {                                                                                  \
        const auto& bf_actual_ = (actual);                                                \
        const auto& bf_unexpected_ = (unexpected);                                        \
        if (bf_actual_ == bf_unexpected_) {                                               \
            ::bftest::fail(__FILE__, __LINE__, #actual " != " #unexpected,                \
                           "both=" + ::bftest::describe(bf_actual_));                     \
        }                                                                                 \
    } while (false)

#define BF_CHECK_CODE(result, expected_code)                                              \
    do {                                                                                  \
        const auto& bf_result_ = (result);                                                \
        if (bf_result_.code() != (expected_code)) {                                       \
            ::bftest::fail(__FILE__, __LINE__, #result " code == " #expected_code,        \
                           "actual=" + std::string(::buffer_fabric::to_string(bf_result_.code())) + \
                               " message=" + bf_result_.status().to_string());            \
        }                                                                                 \
    } while (false)

#define BF_TEST_MAIN()                                                                    \
    int main(int argc, char** argv) { return ::bftest::run_all(argc, argv); }
