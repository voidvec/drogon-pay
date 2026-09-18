# Opt-in gcov instrumentation for the coverage workflow
# (DROGON_PAY_COVERAGE, set by the linux-coverage preset).
#
# Four guards, learned the hard way upstream:
#   1. GCC/Clang only — MSVC has no gcov; clang-cl simulates MSVC so it is
#      excluded via CMAKE_CXX_SIMULATE_ID, not the compiler id.
#   2. Debug single-config only — Release optimized line attribution makes
#      the ratchet numbers meaningless, and multi-config generators scatter
#      .gcda/.gcno across config dirs.
#   3. Static/object libraries are instrumented at compile time; the gcov
#      runtime lands when the final executable links with --coverage.
#   4. Everything is PRIVATE so consumer builds never inherit it.

include_guard(GLOBAL)

function(pay_apply_gcov target)
  if(NOT DROGON_PAY_COVERAGE)
    return()
  endif()
  if(NOT TARGET ${target})
    message(FATAL_ERROR "pay_apply_gcov: no such target '${target}'")
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
     OR CMAKE_CXX_SIMULATE_ID MATCHES "MSVC")
    message(STATUS
      "pay_apply_gcov(${target}): no-op — gcov instrumentation needs "
      "GCC/Clang (clang-cl excluded), got '${CMAKE_CXX_COMPILER_ID}'")
    return()
  endif()

  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
      "DROGON_PAY_COVERAGE=ON requires CMAKE_BUILD_TYPE=Debug "
      "(got '${CMAKE_BUILD_TYPE}'); use the linux-coverage preset")
  endif()

  target_compile_options(${target} PRIVATE --coverage)
  get_target_property(target_type ${target} TYPE)
  if(target_type STREQUAL "EXECUTABLE")
    target_link_options(${target} PRIVATE --coverage)
  endif()
endfunction()
