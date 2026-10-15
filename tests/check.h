#pragma once

// A tiny test harness, no dependencies: TEST_CASE registers a function, CHECK counts and reports, REQUIRE also ends the
// test case. Results are counted per test file (one file per header), main.cpp prints them.
//
//   TEST_CASE("callbacks: a module's load callback fires")
//   {
//       CHECK(x == 1);
//   }

#include <algorithm>
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace check
{
    struct Case
    {
        const char* name;
        const char* file;
        void (*fn)();
    };

    struct Totals
    {
        int cases = 0, checks = 0, failures = 0;
    };

    struct RequireFailed {};

    inline std::vector<Case>& Cases()
    {
        static std::vector<Case> cases;
        return cases;
    }

    inline Totals& Current()
    {
        static Totals totals;
        return totals;
    }

    inline const char*& CurrentCase()
    {
        static const char* name = "";
        return name;
    }

    struct Registrar
    {
        Registrar(const char* name, const char* file, void (*fn)())
        {
            Cases().push_back({ name, file, fn });
        }
    };

    inline bool Check(bool condition, const char* what, const char* file, int line)
    {
        ++Current().checks;
        if (!condition)
        {
            ++Current().failures;
            std::printf("FAILED %s(%d) [%s]: %s\n", file, line, CurrentCase(), what);
            std::fflush(stdout);
        }
        return condition;
    }

    // The file name without its folder and extension: the suite
    inline std::string Suite(const char* file)
    {
        std::string s = file;
        auto slash = s.find_last_of("/\\");
        if (slash != std::string::npos)
            s = s.substr(slash + 1);
        auto dot = s.find_last_of('.');
        return dot == std::string::npos ? s : s.substr(0, dot);
    }

    // Runs every test case (or those whose suite is in only), prints per suite, returns the number of failures
    inline int RunAll(const std::vector<std::string>& only = {})
    {
        std::map<std::string, Totals> suites;
        Totals all;
        for (auto& c : Cases())
        {
            auto suite = Suite(c.file);
            if (!only.empty() && std::find(only.begin(), only.end(), suite) == only.end())
                continue;
            Current() = {};
            CurrentCase() = c.name;
            try
            {
                c.fn();
            }
            catch (RequireFailed&)
            {
            }
            catch (std::exception& e)
            {
                ++Current().failures;
                std::printf("FAILED [%s]: exception %s\n", c.name, e.what());
            }
            catch (...)
            {
                ++Current().failures;
                std::printf("FAILED [%s]: unknown exception\n", c.name);
            }
            auto& t = suites[suite];
            ++t.cases;
            t.checks += Current().checks;
            t.failures += Current().failures;
            ++all.cases;
            all.checks += Current().checks;
            all.failures += Current().failures;
        }
        for (auto& [name, t] : suites)
            std::printf("%-20s %3d test cases %4d checks %s\n", name.c_str(), t.cases, t.checks, t.failures ? "FAILED" : "ok");
        std::printf("%-20s %3d test cases %4d checks, %d failed\n", "total", all.cases, all.checks, all.failures);
        return all.failures;
    }
}

#define CHECK_CONCAT2(a, b) a##b
#define CHECK_CONCAT(a, b) CHECK_CONCAT2(a, b)
#define TEST_CASE(name) \
    static void CHECK_CONCAT(check_case_, __LINE__)(); \
    static check::Registrar CHECK_CONCAT(check_registrar_, __LINE__)(name, __FILE__, &CHECK_CONCAT(check_case_, __LINE__)); \
    static void CHECK_CONCAT(check_case_, __LINE__)()
#define CHECK(x) check::Check(static_cast<bool>(x), #x, __FILE__, __LINE__)
#define REQUIRE(x) do { if (!check::Check(static_cast<bool>(x), #x, __FILE__, __LINE__)) throw check::RequireFailed{}; } while (0)
#define CHECK_THROWS(expr) do { bool thrown_ = false; try { (void)(expr); } catch (...) { thrown_ = true; } check::Check(thrown_, "throws: " #expr, __FILE__, __LINE__); } while (0)
