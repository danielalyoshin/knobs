// SPDX-License-Identifier: GPL-2.0-or-later
//
// A DLL that imports knobs-test-dep.dll, which isn't next to it, the way a VST
// plugin can need a DLL knobs's copy of OBS doesn't have.

extern "C" __declspec(dllimport) int KnobsTestDep();

extern "C" __declspec(dllexport) int KnobsTestNeedsDep() { return KnobsTestDep(); }
