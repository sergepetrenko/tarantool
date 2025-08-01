set(CMAKE_CXX_STANDARD 14)

set(PERF_OUTPUT_DIR ${PROJECT_BINARY_DIR}/perf/output)
file(MAKE_DIRECTORY ${PERF_OUTPUT_DIR})

set(BENCH_RESULTS "")
set(TARANTOOL_BIN $<TARGET_FILE:tarantool>)

if (DEFINED ENV{BENCH_CMD})
    set(BENCH_CMD "$ENV{BENCH_CMD}")
endif()

# If the test output (stdout or stderr) matches these regular
# expressions the test will fail, regardless of the process exit
# code.
list(APPEND CTEST_FAIL_REGEXP
  # Google Benchmark will produce a warning below if it was linked
  # with a library built with enabled debug.
  "Library was built as DEBUG\\. Timings may be affected\\."
)

# Parse command-line arguments into a semicolon-separated list.
separate_arguments(BENCH_CMD_SEPARATE UNIX_COMMAND ${BENCH_CMD})

set(TEST_SUITE_NAME "performance-c")

list(APPEND TARANTOOL_PERF_CTEST_FLAGS
    --parallel 0
    # Always show test output with results.
    --verbose
    --output-on-failure
    --no-tests=error
)

add_subdirectory(lua)

if(ENABLE_BUNDLED_LIBBENCHMARK)
    include(BuildBenchmark)
    add_dependencies(build_bundled_libs bundled-libbenchmark)
    include_directories(${BENCHMARK_INCLUDE_DIRS})
else()
    find_package(benchmark QUIET)
    set(BENCHMARK_LIBRARIES benchmark::benchmark)
endif()

# Always set for bundled libbenchmark.
if(NOT ${benchmark_FOUND})
    message(AUTHOR_WARNING "Google Benchmark library was not found")
    set(MSG "Target test-c-perf is dummy, Google Benchmark library was not found")
    add_custom_target(test-c-perf
                      COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --red ${MSG}
                      COMMENT ${MSG}
    )
    add_custom_target(test-perf
                      DEPENDS test-c-perf test-lua-perf
                      COMMENT "Running performance tests"
    )
    return()
endif()

include_directories(${MSGPUCK_INCLUDE_DIRS})
include_directories(${PROJECT_SOURCE_DIR}/src/box)
include_directories(${PROJECT_SOURCE_DIR}/third_party)
include_directories(${EXTRA_BOX_INCLUDE_DIRS})
include_directories(${EXTRA_CORE_INCLUDE_DIRS})

set(RUN_PERF_C_TESTS_LIST "")

message(STATUS "Add test suite ${TEST_SUITE_NAME}")

# XXX: The call produces both test and target
# <${TEST_SUITE_NAME}-deps> as a side effect.
_add_test_suite_target(${TEST_SUITE_NAME}
  LABELS "${TEST_SUITE_NAME};performance;"
  DEPENDS ${RUN_PERF_C_TESTS_LIST}
)

function(create_perf_test_target)
  set(prefix PERF)
  set(noValues)
  set(singleValues TARGET)
  set(multiValues)

  # FIXME: if we update to CMake >= 3.5, can remove this line.
  include(CMakeParseArguments)
  cmake_parse_arguments(${prefix}
                        "${noValues}"
                        "${singleValues}"
                        "${multiValues}"
                        ${ARGN})

  set(BENCH_RESULT ${PERF_OUTPUT_DIR}/${PERF_TARGET}.json)
  set(BENCH_TARGET ${PERF_TARGET}_perftest)
  set(BENCH_RESULT_TARGET ${BENCH_TARGET}_result)

  # XXX: We need to provide two different targets with the same
  # command: the first (BENCH_TARGET) is run unconditionally
  # regardless of whether there are files with benchmark results
  # or not, and the second target (BENCH_RESULT_TARGET) is run
  # only if the corresponding file is omitted. The COMMAND_LIST
  # variable contains the same command for these targets.
  set(COMMAND_LIST
        COMMAND ${BENCH_CMD_SEPARATE} $<TARGET_FILE:${PERF_TARGET}.perftest>
                --benchmark_out_format=json
                --benchmark_out="${BENCH_RESULT}"
        DEPENDS ${PERF_TARGET}.perftest
        COMMENT Running ${BENCH_TARGET}
  )
  add_custom_command(OUTPUT ${BENCH_RESULT} ${COMMAND_LIST})
  add_custom_target(${BENCH_RESULT_TARGET} DEPENDS ${BENCH_RESULT})
  add_custom_target(${BENCH_TARGET} ${COMMAND_LIST})

  set(RUN_PERF_C_TESTS_LIST ${RUN_PERF_C_TESTS_LIST} ${BENCH_TARGET} PARENT_SCOPE)
  add_dependencies(${TEST_SUITE_NAME}-deps ${PERF_TARGET}.perftest)
  set(BENCH_RESULTS ${BENCH_RESULT_TARGET} ${BENCH_RESULTS} PARENT_SCOPE)
  set(test_title "perf/${PERF_TARGET}.cc")
  add_test(NAME ${test_title}
           COMMAND "$<TARGET_FILE:${PERF_TARGET}.perftest>"
                   --benchmark_out_format=json
                   --benchmark_out=${BENCH_RESULT}
  )
  set_tests_properties(${test_title} PROPERTIES
    LABELS "${TEST_SUITE_NAME};performance;"
    DEPENDS ${TEST_SUITE_NAME}-deps
    FAIL_REGULAR_EXPRESSION "${CTEST_FAIL_REGEXP}"
  )
endfunction()

function(create_perf_test)
  set(prefix PERF)
  set(noValues)
  set(singleValues NAME)
  set(multiValues "SOURCES;LIBRARIES")

  # FIXME: if we update to CMake >= 3.5, can remove this line.
  include(CMakeParseArguments)
  cmake_parse_arguments(${prefix}
                        "${noValues}"
                        "${singleValues}"
                        "${multiValues}"
                        ${ARGN})
  add_executable(${PERF_NAME}.perftest ${PERF_SOURCES})
  target_link_libraries(${PERF_NAME}.perftest PUBLIC ${PERF_LIBRARIES} pthread)
endfunction()

create_perf_test(NAME tuple
                 SOURCES tuple.cc ${PROJECT_SOURCE_DIR}/test/unit/box_test_utils.c
                 LIBRARIES core box tuple ${BENCHMARK_LIBRARIES}
)
create_perf_test_target(TARGET tuple)

create_perf_test(NAME bps_tree
                 SOURCES bps_tree.cc ${PROJECT_SOURCE_DIR}/test/unit/box_test_utils.c
                 LIBRARIES core box tuple ${BENCHMARK_LIBRARIES}
)
create_perf_test_target(TARGET bps_tree)

create_perf_test(NAME light
                 SOURCES light.cc ${PROJECT_SOURCE_DIR}/test/unit/box_test_utils.c
                 LIBRARIES small ${BENCHMARK_LIBRARIES}
)
create_perf_test_target(TARGET light)

create_perf_test_target(TARGET small)

create_perf_test(NAME memtx
                 SOURCES memtx.cc ${PROJECT_SOURCE_DIR}/test/unit/box_test_utils.c
                 LIBRARIES core box server ${BENCHMARK_LIBRARIES}
)
create_perf_test_target(TARGET memtx)

add_custom_target(test-c-perf
                  COMMAND ${CMAKE_CTEST_COMMAND}
                          ${TARANTOOL_PERF_CTEST_FLAGS}
                          -L ${TEST_SUITE_NAME}
                  DEPENDS ${RUN_PERF_C_TESTS_LIST}
                  COMMENT "Running C performance tests"
)

add_custom_target(test-perf
                  COMMAND ${CMAKE_CTEST_COMMAND}
                          ${TARANTOOL_PERF_CTEST_FLAGS}
                          -L performance
                  COMMENT "Running all performance tests"
)


set(PERF_SUMMARY ${PERF_OUTPUT_DIR}/summary.txt)
set(PERF_CONFIG ${PERF_OUTPUT_DIR}/config_bench.json)
add_custom_target(test-perf-aggregate
                  DEPENDS ${BENCH_RESULTS}
                  BYPRODUCTS ${PERF_SUMMARY}
                  COMMENT "Aggregate performance test results into ${PERF_SUMMARY}"
                  COMMAND ${TARANTOOL_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/tools/aggregate.lua
                    --output=${PERF_SUMMARY}
                    --input_dir=${PERF_OUTPUT_DIR}
                    --config_path=${PERF_CONFIG}
                  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
