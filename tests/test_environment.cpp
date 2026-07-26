#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <string>

#include "core/environment.h"
#include "core/preferences.h"

namespace {

// setenv is POSIX and absent from the Microsoft runtime, whose own annex
// deletes a variable when given an empty value.
void setVariable(const char* name, const char* value)
{
#ifdef _MSC_VER
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void clearVariable(const char* name)
{
#ifdef _MSC_VER
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

}  // namespace

TEST_CASE("an unset variable reads as empty", "[environment]")
{
    clearVariable("SIDESCOPES_TEST_VARIABLE");

    CHECK(sidescopes::environmentValue("SIDESCOPES_TEST_VARIABLE").empty());
}

TEST_CASE("a set variable reads back", "[environment]")
{
    setVariable("SIDESCOPES_TEST_VARIABLE", "a value with spaces");

    CHECK(sidescopes::environmentValue("SIDESCOPES_TEST_VARIABLE") == "a value with spaces");

    clearVariable("SIDESCOPES_TEST_VARIABLE");
    CHECK(sidescopes::environmentValue("SIDESCOPES_TEST_VARIABLE").empty());
}

TEST_CASE("the preferences file follows its environment override", "[environment][preferences]")
{
    clearVariable(sidescopes::PreferencesFileVariable);
    CHECK(sidescopes::preferencesFileFromEnvironment().empty());

    setVariable(sidescopes::PreferencesFileVariable, "/tmp/sidescopes-scratch/preferences.txt");
    CHECK(sidescopes::preferencesFileFromEnvironment() == "/tmp/sidescopes-scratch/preferences.txt");

    clearVariable(sidescopes::PreferencesFileVariable);
    CHECK(sidescopes::preferencesFileFromEnvironment().empty());
}
