#pragma once
// Minimal test helpers: no framework dependency, runs anywhere g++ does.
#include <cstdio>

namespace test
{
extern int g_failures;
extern int g_checks;

/** Records one check. On failure prints the location and a message. */
bool Check(bool ok, const char *file, int line, const char *what);

/** Registry of test functions (filled by TEST(...) at static init). */
struct Case
{
    const char *name;
    void (*fn)();
    Case       *next;
};
Case *&Head();
struct Register
{
    Register(Case *c)
    {
        c->next = Head();
        Head()  = c;
    }
};
} // namespace test

#define CHECK(cond) test::Check((cond), __FILE__, __LINE__, #cond)

// Check with a printf-style message describing the failing case.
#define CHECKF(cond, ...)                                     \
    do                                                        \
    {                                                         \
        if(!test::Check((cond), __FILE__, __LINE__, #cond))   \
        {                                                     \
            std::printf("      ");                            \
            std::printf(__VA_ARGS__);                         \
            std::printf("\n");                                \
        }                                                     \
    } while(0)

#define TEST(name)                                                \
    static void       name();                                     \
    static test::Case name##_case = {#name, name, nullptr};       \
    static test::Register name##_reg(&name##_case);               \
    static void       name()
