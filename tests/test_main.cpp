#include <cstdio>

#include "test_framework.hpp"

#if defined(_WIN32) && defined(_DEBUG)
#  include <crtdbg.h>
#  include <cstdlib>
#endif

namespace {

/// Install fail-fast reporting for the Microsoft C runtime. A debug assertion
/// or invalid-parameter report must terminate the process and print, never open
/// a modal dialog: a blocked test is indistinguishable from a hang.
///
/// The Microsoft debug report types only exist in a debug runtime, so this is
/// compiled and used only when _DEBUG is defined.
void install_crt_reporting() {
#if defined(_WIN32) && defined(_DEBUG)
    ::_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    ::_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    ::_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    ::_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    install_crt_reporting();
    return ::bftest::run_all(argc, argv);
}
