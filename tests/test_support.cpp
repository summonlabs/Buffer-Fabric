#include "test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace bftest {
namespace {

std::string make_unique_path(const std::string& tag) {
    static std::atomic<u64> counter{0};
    const u64 sequence = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    std::string base;
    if (const char* temp = std::getenv("TEMP")) base = temp;
    if (base.empty()) {
        if (const char* temp = std::getenv("TMP")) base = temp;
    }
#if defined(_WIN32)
    if (base.empty()) base = "C:/Windows/Temp";
#else
    if (base.empty()) base = "/tmp";
#endif
#if defined(_WIN32)
    const long process_id = static_cast<long>(::_getpid());
#else
    const long process_id = static_cast<long>(::getpid());
#endif
    char buffer[600];
    std::snprintf(buffer, sizeof(buffer), "%s/buffer_fabric_test_%s_%ld_%llu", base.c_str(),
                  tag.c_str(), process_id, static_cast<unsigned long long>(sequence));
    return std::string(buffer);
}

}  // namespace

TempDirectory::TempDirectory(const std::string& tag) : path_(make_unique_path(tag)) {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
}

TempDirectory::~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

std::string TempDirectory::file(const std::string& leaf) const { return path_ + "/" + leaf; }

}  // namespace bftest
