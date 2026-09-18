# ##############################################################################
# Example host: PayServer. Owns everything that is a host concern rather than
# a library concern: .env loading, config placeholder substitution, startup
# validation, CORS/security headers, /healthz and /metrics endpoints.
#
# pay_host_core is an OBJECT library so the test suite can link the same host
# utils/controllers to replicate the host startup flow (tests -> host is an
# allowed dependency direction; the library itself never depends on the host).
# ##############################################################################
add_library(pay_host_core OBJECT
    controllers/HealthCheckController.cc
    controllers/MetricsController.cc
    utils/ConfigLoader.cc
    utils/StartupValidator.cc
)

target_include_directories(pay_host_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(pay_host_core PUBLIC pay_core)

add_executable(PayServer main.cc)

# OBJECT library object files do NOT propagate transitively: linking
# pay_host_core only pulls its own objects, so pay_core must be listed
# explicitly to get the library's objects into the executable.
target_link_libraries(PayServer PRIVATE pay_host_core pay_core)

# First-party warning bar (shared with the library; see cmake/Warnings.cmake).
include(Warnings)
pay_apply_warnings(pay_host_core)
pay_apply_warnings(PayServer)

# Opt-in gcov instrumentation (DROGON_PAY_COVERAGE). PayServer must be
# instrumented too: it links the same instrumented pay_host_core objects and
# needs the gcov runtime at link time.
include(Coverage)
pay_apply_gcov(pay_host_core)
pay_apply_gcov(PayServer)
