# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs knobs-import against tests/fixtures/obs-config, a made-up OBS settings
# folder, and checks what it reports. Needs OBS installed; prints SKIP when
# it isn't, which the test's SKIP_REGULAR_EXPRESSION turns into a skip. WORK
# is a folder it may replace, for an edited copy of the fixture. OBS_DIR, if
# set, is the OBS to use instead of the installed one.
#
#   cmake -DTOOL=<knobs-import.exe> -DCONFIG=<tests/fixtures/obs-config> -DWORK=<folder>
#         [-DOBS_DIR=<folder>] -P import_fixture.cmake

set(obs_dir)
if(OBS_DIR)
  set(obs_dir --obs-dir "${OBS_DIR}")
endif()

function(run_import out_var code_var)
  execute_process(
    COMMAND "${TOOL}" --obs-config "${CONFIG}" ${obs_dir} ${ARGN}
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

function(expect_not output text)
  string(FIND "${output}" "${text}" at)
  if(NOT at EQUAL -1)
    message(FATAL_ERROR "Didn't expect \"${text}\" in:\n${output}")
  endif()
endfunction()

# Two mics and no --pick: lists them and fails.
run_import(output code)
if(code EQUAL 77)
  message("SKIP (OBS isn't installed)")
  return()
endif()
message("${output}")
if(code EQUAL 0)
  message(FATAL_ERROR "Importing without --pick should fail when there are two mics.")
endif()
# The profile and collection go by the names saved in them, not by folder or
# file names, and [Locations] folders that don't exist fall back.
expect("${output}" "\"Streaming Voice\": 44100 Hz, Mono")
expect("${output}" "\"Main Scenes\":")
expect("${output}" "Main_Scenes.json")
expect("${output}" "1. \"Mic/Aux\" (AuxAudioDevice1, monitored)")
expect("${output}" "2. \"Podcast Mic\" (sources[1])")
expect("${output}" "Pass --pick")

# The mic from "sources": every pre-flight check fires.
run_import(output code --pick "podcast mic")
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "Importing \"Podcast Mic\" failed.")
endif()
# The optional modules load for the filters that need them. The VST plugin
# isn't there, and NVIDIA's SDK isn't either: the test points nv-filters at a
# folder without it (NVAFX_SDK_DIR).
expect("${output}" "[ok  ] modules            loaded obs-vst, which the chain needs")
expect("${output}" "[ok  ] modules            loaded nv-filters, which the chain needs")
expect("${output}" "[--  ] pre-flight         Filter \"VST\" is a VST plugin (ReaComp-standalone.dll) that isn't at C:/Program Files/VSTPlugins/ReaComp-standalone.dll any more, so it passes audio through untouched, in OBS too.")
expect("${output}" "[--  ] pre-flight         Filter \"VST\" opens its VST plugin's window in OBS.")
expect("${output}" "[warn] pre-flight         Filter \"NVIDIA Noise Removal\" is NVIDIA's noise removal, which needs NVIDIA's Audio Effects SDK. It isn't installed")
expect("${output}" "[warn] pre-flight         Compressor \"Ducking\" turns the mic down under \"Music\" in OBS.")
expect("${output}" " loads only the mic, so it keeps only this compressor's output gain (+2.5 dB), and the mic sounds as it does in OBS while nothing plays on \"Music\".")
expect("${output}" "[--  ] pre-flight         Filter \"Gate\" is off in OBS, and stays off.")
expect("${output}" "[--  ] pre-flight         Filter \"Third-Party\" (some_plugin_filter) isn't one ")
expect("${output}" " has, but it's off in OBS anyway.")
expect("${output}" "[--  ] pre-flight         Filter \"Old VST\" is off in OBS, and stays off.")
expect("${output}" "The mic is muted and on push-to-talk in OBS, which doesn't silence the cable. Set up push-to-talk in each app that uses the cable.")
expect("${output}" "sync offset of 50 ms")
expect("${output}" "OBS doesn't monitor this mic")
# The compressor with a sidechain is a Gain filter at its output gain, in its
# place; the rest load in order, with source-level state, the VST filters as
# obs-vst's.
expect("${output}" "balance 0.30, Mono off, then 7 filter(s), then volume 0.50")
expect("${output}" "1. vst_filter \"VST\"")
expect_not("${output}" "1. vst_filter \"VST\" (unknown")
expect("${output}" "2. nvidia_audiofx_filter \"NVIDIA Noise Removal\" (unknown to libobs")
expect("${output}" "3. gain_filter \"Ducking\"")
expect("${output}" "6. gain_filter \"Makeup\"")
expect("${output}" "7. vst_filter \"Old VST\" (disabled)")
expect_not("${output}" "compressor_filter")
expect("${output}" "libobs loaded the chain and ran its load callbacks; 2 warning(s)")

# The global Mic/Aux device: no load callbacks, nothing to warn about.
run_import(output code --pick 1)
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "Importing \"Mic/Aux\" failed.")
endif()
expect("${output}" "[ok  ] pre-flight         nothing to report")
expect("${output}" "balance 0.50, Mono on, then 2 filter(s), then volume 1.00")
expect("${output}" "1. noise_suppress_filter_v2 \"Noise Suppression\"")
expect("${output}" "libobs loaded the chain; 0 warning(s)")
string(REGEX MATCH "chain key +[0-9a-f]+" key "${output}")
if(NOT key)
  message(FATAL_ERROR "Expected a chain key in:\n${output}")
endif()

# Names and filter UUIDs don't change what reaches the cable, so renaming the
# mic and a filter, or adding the filter again, leaves the chain key as it
# was. A setting changes it.
file(REMOVE_RECURSE "${WORK}")
file(COPY "${CONFIG}/" DESTINATION "${WORK}")
set(scenes "${WORK}/obs-studio/basic/scenes/Main_Scenes.json")
file(READ "${scenes}" json)
string(REPLACE "\"name\": \"Mic/Aux\"" "\"name\": \"Mic\"" json "${json}")
string(REPLACE "\"name\": \"Gain\"" "\"name\": \"Boost\"" json "${json}")
string(REPLACE "a5d0c3a4-3f7e-4a43-9f0a-000000000004" "a5d0c3a4-3f7e-4a43-9f0a-0000000000f4" json "${json}")
file(WRITE "${scenes}" "${json}")
# From here on, import from the copy.
set(CONFIG "${WORK}")
run_import(output code --pick 1)
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "Importing the renamed \"Mic\" failed.")
endif()
expect("${output}" "1. \"Mic\" (AuxAudioDevice1, monitored)")
expect("${output}" "2. gain_filter \"Boost\"")
expect("${output}" "${key}")

string(REPLACE "\"db\": 3.0" "\"db\": 4.0" json "${json}")
file(WRITE "${scenes}" "${json}")
run_import(output code --pick 1)
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "Importing \"Mic\" with a new gain failed.")
endif()
expect_not("${output}" "${key}")
