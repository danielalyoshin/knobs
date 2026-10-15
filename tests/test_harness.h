// SPDX-License-Identifier: GPL-2.0-or-later
//
// A deliberately tiny test harness: TEST registers, CHECK records. Test
// files define their tests in an anonymous namespace; unit_tests.cpp runs
// them all.
#pragma once

#include <cstdio>
#include <string_view>
#include <vector>

namespace knobs::test {

struct TestCase {
  const char* name;
  void (*run)();
};

inline std::vector<TestCase>& Tests() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, void (*run)()) { Tests().push_back({name, run}); }
};

inline int g_failures = 0;

// Whether `text` has no straight quotes. knobs's own text is written with
// typographic ones (’ “ ”), and the tests' made-up names have none, so a
// straight one is text that was written without them.
inline bool NoStraightQuotes(std::string_view text) { return text.find_first_of("'\"") == std::string_view::npos; }

// Whether this binary was started as the stand-in for OBS that a tray test
// starts (tray_tests.cpp, OpenObsStartsInItsBinFolder). It has done its part
// when this returns true, and should exit.
bool RunAsStandInObs();

// Exercises the tray's real quit path in a child process, with no libobs or
// audio devices. A stuck core is deliberately terminated (shutdown_tests.cpp).
int RunShutdownHelper(const wchar_t* mode, const wchar_t* folder);

}  // namespace knobs::test

#define TEST(name)                                            \
  void name();                                                \
  const ::knobs::test::Registrar name##_registrar(#name, &name); \
  void name()

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s(%d): CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      ++::knobs::test::g_failures;                                                        \
    }                                                                                     \
  } while (false)
