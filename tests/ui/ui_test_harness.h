#pragma once

// Shared null-backend runner for the Dear ImGui Test Engine feature suites,
// generalized from tests/ui/test_smoke.cpp so each suite is just its test
// registrations plus a one-line main(). The smoke test keeps its own inline
// copy on purpose; this only serves the newer feature harnesses.

struct ImGuiTestEngine;

namespace sidescopes::uitest {

/// Registers a suite's tests into @p engine. Called once, after the engine
/// and the hooked Dear ImGui context exist.
using RegisterFn = void (*)(ImGuiTestEngine* engine);

/// Boots Dear ImGui on the null (headless) backend with the Test Engine,
/// registers and runs one suite, prints the result summary, and tears it all
/// down. Returns 0 only when exactly @p expectedTests ran and every one
/// passed; non-zero (with a diagnostic on stderr) otherwise, so it can be a
/// program's exit code.
int runSuite(const char* suiteName, RegisterFn registerTests, int expectedTests);

}  // namespace sidescopes::uitest
