# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs knobs-core against tests/fixtures/obs-config, a made-up OBS settings
# folder, and checks the states it reports. A dry run that doesn't watch for
# OBS: no audio device is opened, and OBS running alongside doesn't change the
# outcome. Needs OBS installed; prints SKIP when it isn't, which the test's
# SKIP_REGULAR_EXPRESSION turns into a skip. OBS_DIR, if set, is the OBS to
# use instead of the installed one.
#
#   cmake -DTOOL=<knobs-core.exe> -DCONFIG=<tests/fixtures/obs-config> [-DOBS_DIR=<folder>]
#         -P core_fixture.cmake

set(obs_dir)
if(OBS_DIR)
  set(obs_dir --obs-dir "${OBS_DIR}")
endif()

function(run_core out_var code_var)
  execute_process(
    COMMAND "${TOOL}" --obs-config "${CONFIG}" --ignore-obs --seconds 2 ${obs_dir} ${ARGN}
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
    RESULT_VARIABLE code)
  set(${out_var} "${output}" PARENT_SCOPE)
  set(${code_var} "${code}" PARENT_SCOPE)
endfunction()

function(expect output text)
  string(FIND "${output}" "${text}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "Expected \"${text}\" in:\n${output}")
  endif()
endfunction()

# Two mics and no --pick: the core needs the user to pick one, and loads
# nothing.
run_core(output code)
if(code EQUAL 77)
  message("SKIP (OBS isn't installed)")
  return()
endif()
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "knobs-core failed.")
endif()
expect("${output}" "s] starting")
expect("${output}" "s] needs setup: The scene collection has 2 mics. Choose one:")
expect("${output}" "1. \"Mic/Aux\" (AuxAudioDevice1, monitored)")
expect("${output}" "0 leaked allocations")

# The selected mic's missing VST file is checked on a worker. Its result
# completes the import, with the same pre-flight as knobs-import, while no
# mic or cable opens. NVIDIA's SDK is absent in this test's environment.
run_core(output code --pick 2 --seconds 4)
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "The core failed while checking the selected mic's plugins.")
endif()
expect("${output}" "Filter \"VST\" is a VST plugin (ReaComp-standalone.dll) that isn't at")
expect("${output}" "s] cable missing: \"CABLE Input (VB-Audio Virtual Cable)\" isn't connected.")
expect("${output}" "0 leaked allocations")

# Mic/Aux: its device and the profile's cable are both made up, so neither is
# connected. The cable comes first, under the name the profile saved.
run_core(output code --pick 1)
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "knobs-core --pick 1 failed.")
endif()
expect("${output}" "s] cable missing: \"CABLE Input (VB-Audio Virtual Cable)\" isn't connected.")
expect("${output}" "0 leaked allocations")
