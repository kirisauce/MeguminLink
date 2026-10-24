#pragma once
#ifndef __MLINK_TEST_COMMON_HPP__
#define __MLINK_TEST_COMMON_HPP__

#include <cstddef>
#include <format>
#include <print>
#include <source_location>
#include <string>
#include <string_view>

namespace test
{
inline int g_pass = 0;
inline int g_fail = 0;

/** 记一次断言；失败时打印位置，detail 非空则附在后面 */
inline void check(bool ok, std::source_location where = std::source_location::current(), std::string_view detail = {})
{
    if (ok)
    {
        ++g_pass;
        return;
    }

    ++g_fail;
    if (detail.empty())
    {
        std::println("    [FAIL] {}:{}", where.file_name(), where.line());
        return;
    }
    std::println("    [FAIL] {}:{}  {}", where.file_name(), where.line(), detail);
}

/** 相等断言：失败时用 detail 带上两边的值 */
template <typename Actual, typename Expected>
void check_eq(Actual actual, Expected expected, std::source_location where = std::source_location::current())
{
    const long long lhs = static_cast<long long>(actual);
    const long long rhs = static_cast<long long>(expected);
    const std::string detail = (lhs == rhs) ? std::string{} : std::format("got {}, want {}", lhs, rhs);
    check(lhs == rhs, where, detail);
}

/** 一个用例：名字 + 入口 */
struct Case
{
    const char *name;
    void (*run)();
};

/** 逐个跑用例并打印每例结果，返回进程退出码 */
inline int run(const Case *cases, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        const int before = g_fail;
        std::println("== {}", cases[i].name);
        cases[i].run();
        std::println("   {}", (g_fail == before) ? "PASS" : "FAIL");
    }
    std::println("\nassertions: {} passed, {} failed", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
} // namespace test

#endif // __MLINK_TEST_COMMON_HPP__
