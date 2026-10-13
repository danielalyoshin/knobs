# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs knobs-harness on a chain with one VST filter, knobs-test-vst
# (tests/fixtures/vst), and checks that OBS's VST filter loads the plugin
# with its saved parameters, gives the same output on every run, opens and
# closes it on one thread, and never opens its editor. Needs OBS installed;
# prints SKIP when it isn't, which the test's SKIP_REGULAR_EXPRESSION turns
# into a skip. WORK is a folder it may replace. OBS_DIR, if set, is the OBS
# to use instead of the installed one.
#
#   cmake -DTOOL=<knobs-harness.exe> -DPLUGIN=<knobs-test-vst.dll> -DWORK=<folder>
#         [-DOBS_DIR=<folder>] -P vst_fixture.cmake

set(obs_dir)
if(OBS_DIR)
  set(obs_dir --obs-dir "${OBS_DIR}")
endif()

function(expect output text)
  string(FIND "${output}" "${text}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "Expected \"${text}\" in:\n${output}")
  endif()
endfunction()

# The sha256 of run 1's output, which the harness prints.
function(output_hash out_var output)
  string(REGEX MATCH "run 1 +[^\n]* sha256 ([0-9a-f]+)" match "${output}")
  if(NOT match)
    message(FATAL_ERROR "Expected run 1's sha256 in:\n${output}")
  endif()
  set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(log "${WORK}/knobs-test-vst.log")
file(TO_CMAKE_PATH "${PLUGIN}" plugin)

# The chain, as an OBS source object. obs-vst saves a plugin with no chunks
# as base64 of its raw float parameters: "AACAPgAAAD8=" is 0.25 (Gain: x0.5)
# then 0.5 (Smooth), as little-endian floats. An empty chunk_hash applies
# them whatever the plugin file's MD5. The filter also asks for the plugin's
# interface to open when it's active, which knobs must turn off: obs-vst
# would build a Qt window for it, with no Qt application to show it in.
string(CONFIGURE [=[{
  "id": "knobs_push_source", "name": "VST test",
  "filters": [
    {"id": "vst_filter", "versioned_id": "vst_filter", "name": "Test VST",
     "settings": {"plugin_path": "@plugin@", "chunk_data": "AACAPgAAAD8=", "chunk_hash": "",
                  "open_when_active_vst_settings": true}}
  ]
}
]=] source @ONLY)
file(WRITE "${WORK}/source.json" "${source}")

# Three runs of the built-in test signal, each with a new instance of the
# plugin.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "KNOBS_TEST_VST_LOG=${log}"
          "${TOOL}" --source "${WORK}/source.json" --runs 3 --out "${WORK}/vst.wav" ${obs_dir}
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output
  RESULT_VARIABLE code)
if(code EQUAL 77)
  message("SKIP (OBS isn't installed)")
  return()
endif()
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "The harness failed on the VST chain.")
endif()
# The filter loads as itself, with no note after it: libobs knows it, it's
# an audio filter, and it's on.
if(NOT output MATCHES "1\\. vst_filter \"Test VST\"\r?\n")
  message(FATAL_ERROR "Expected the chain to be the VST filter, as libobs knows it, in:\n${output}")
endif()
foreach(run 2 3)
  if(NOT output MATCHES "\\[ok  \\] run ${run} +bit-identical to run 1")
    message(FATAL_ERROR "Expected run ${run} to be bit-identical to run 1 in:\n${output}")
  endif()
endforeach()
expect("${output}" "0 leaked allocations")
output_hash(vst_hash "${output}")

# One open and one close per run, all on the harness's thread, which runs
# the chain, and no editor. Each run's plugin closes before the next opens:
# knobs has Qt delete it as the run's chain goes, not only at shutdown.
if(NOT EXISTS "${log}")
  message(FATAL_ERROR "The plugin wrote no log, so obs-vst never opened it.")
endif()
file(READ "${log}" log_text)
file(STRINGS "${log}" lines)
set(opens 0)
set(closes 0)
set(threads)
foreach(line IN LISTS lines)
  if(line MATCHES "^open ([0-9]+)$")
    if(NOT opens EQUAL closes)
      message(FATAL_ERROR "A run's plugin opened before the last run's closed:\n${log_text}")
    endif()
    math(EXPR opens "${opens} + 1")
    list(APPEND threads "${CMAKE_MATCH_1}")
  elseif(line MATCHES "^close ([0-9]+)$")
    math(EXPR closes "${closes} + 1")
    list(APPEND threads "${CMAKE_MATCH_1}")
  elseif(line MATCHES "^close-wrong-thread ")
    message(FATAL_ERROR "obs-vst closed the plugin on another thread than the one that opened it:\n${log_text}")
  elseif(line MATCHES "^editor ")
    message(FATAL_ERROR "obs-vst called the plugin's editor:\n${log_text}")
  else()
    message(FATAL_ERROR "Unexpected line \"${line}\" in the plugin's log:\n${log_text}")
  endif()
endforeach()
if(NOT opens EQUAL 3 OR NOT closes EQUAL 3)
  message(FATAL_ERROR "Expected 3 opens and 3 closes, one of each per run, in the plugin's log:\n${log_text}")
endif()
list(REMOVE_DUPLICATES threads)
list(LENGTH threads thread_count)
if(NOT thread_count EQUAL 1)
  message(FATAL_ERROR "Expected every open and close on one thread in the plugin's log:\n${log_text}")
endif()

# The same signal through a 0 dB gain filter: the plugin changed the audio.
execute_process(
  COMMAND "${TOOL}" --chain gain --runs 1 ${obs_dir}
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output
  RESULT_VARIABLE code)
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "The harness failed on the gain chain.")
endif()
output_hash(gain_hash "${output}")
if(vst_hash STREQUAL gain_hash)
  message(FATAL_ERROR "The VST chain's output is the same as a 0 dB gain filter's: the plugin did nothing.")
endif()
