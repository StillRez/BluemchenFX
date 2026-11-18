// Host test runner: `make -C tests run`. Exit code 0 = all checks passed.
// Optional argument: run only tests whose name contains it.
#include "check.h"
#include <cstring>
#include <chrono>

namespace test
{
int g_failures = 0;
int g_checks   = 0;

static const char *g_current = "";

bool Check(bool ok, const char *file, int line, const char *what)
{
    g_checks++;
    if(!ok)
    {
        g_failures++;
        // Keep the log readable if one bug fails thousands of checks
        if(g_failures <= 50)
            std::printf("  FAIL [%s] %s:%d: %s\n", g_current, file, line, what);
    }
    return ok;
}

Case *&Head()
{
    static Case *head = nullptr;
    return head;
}
} // namespace test

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : nullptr;

    // Registration order is reversed; restore source order
    test::Case *list[256];
    int         n = 0;
    for(test::Case *c = test::Head(); c && n < 256; c = c->next)
        list[n++] = c;

    int run = 0;
    for(int i = n - 1; i >= 0; i--)
    {
        test::Case *c = list[i];
        if(filter && !std::strstr(c->name, filter))
            continue;
        test::g_current = c->name;
        const int before = test::g_failures;
        auto      t0     = std::chrono::steady_clock::now();
        c->fn();
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        std::printf("%-4s %s (%.0f ms)\n",
                    test::g_failures == before ? "ok" : "FAIL",
                    c->name,
                    ms);
        run++;
    }

    std::printf("\n%d tests, %d checks, %d failures\n",
                run,
                test::g_checks,
                test::g_failures);
    return test::g_failures == 0 && run > 0 ? 0 : 1;
}
