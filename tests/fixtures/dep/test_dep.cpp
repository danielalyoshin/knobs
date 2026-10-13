// SPDX-License-Identifier: GPL-2.0-or-later
//
// A DLL that knobs-test-needs-dep imports, built into a folder of its own
// that no loader searches, so the unit tests have an import that won't
// resolve (runtime::UnresolvedDllImports).

extern "C" __declspec(dllexport) int KnobsTestDep() { return 1; }
