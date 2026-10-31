#include "test.hpp"

#include <cstring>

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int failedCases = 0, ran = 0;
    for (const auto& c : test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        ++ran;
        int before = test::failures();
        c.fn();
        bool ok = test::failures() == before;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
        if (!ok) ++failedCases;
    }
    std::printf("\n%d tests, %d failed\n", ran, failedCases);
    return failedCases == 0 ? 0 : 1;
}
