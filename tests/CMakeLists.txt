cmake_minimum_required(VERSION 3.21)
project(PayBackendTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TEST_SRC
    main.cc
    # Unit tests: no HTTP/DB round-trip, pure logic of host and library helpers.
    unit/AuthCheckTest.cc
    unit/ConfigLoaderTest.cc
    unit/ControllerMetricsTest.cc
    unit/OnceCallbackTest.cc
    unit/PayAuthMetricsTest.cc
    unit/PayUtilsTest.cc
    unit/AlipayVerifyCallbackTest.cc
    unit/StartupValidatorTest.cc
    # Integration tests: exercise the running Drogon app, DB or HTTP surface.
    integration/CallbackControllerTest.cc    # P0: controller-level callback validation (C1-2, C2-1)
    integration/CreatePaymentIntegrationTest.cc
    integration/HealthProbeTest.cc
    integration/HttpResponseHeadersTest.cc   # P2: CORS preflight + security headers
    integration/IdempotencyIntegrationTest.cc
    integration/PayErrorCategoryTest.cc      # P3-7.2: PayErrorCategory singleton + concurrent
    integration/QueryOrderListAndReconcileTest.cc  # P1: queryOrderList + reconcileSummary params
    integration/QueryOrderTest.cc
    integration/ReconcileSummaryTest.cc
    integration/RefundQueryTest.cc
    integration/RouteRegistrationSmokeTest.cc
    integration/WechatCallbackIntegrationTest.cc
    integration/WechatPayClientTest.cc
)

add_executable(${PROJECT_NAME} ${TEST_SRC})

# TestConfigHelper.h lives at tests/ and is included by tests in both
# subdirectories.
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Business sources come from the pay_core object library defined in
# libs/drogon-pay (no more per-file recompilation). pay_core carries the
# include dirs, Drogon/OpenSSL usage requirements and codecvt silencers as
# PUBLIC properties. pay_host_core adds the example host's utils/controllers
# (ConfigLoader, StartupValidator, health/metrics) which the test main
# replicates the host startup flow with.
target_link_libraries(${PROJECT_NAME} PRIVATE pay_core pay_host_core)

add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../examples/pay-server/config.json"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config.json"
)
# .env is not tracked in git; only copy it when present (local dev). CI injects
# config via environment variables and a clean checkout has no .env, which would
# otherwise make `cmake -E copy_if_different` fail the build.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../examples/pay-server/.env")
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/../examples/pay-server/.env"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.env"
    )
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /bigobj  # Required for large object files (test files with lots of templates)
        /utf-8   # Same source-encoding contract as the library targets
    )
endif()

# Test fixtures mint legacy RSA keys directly (RSA_new/RSA_generate_key_ex);
# the deprecated-API warning is expected noise here and would break the
# DROGON_PAY_WERROR=ON gate.
target_compile_definitions(${PROJECT_NAME} PRIVATE OPENSSL_SUPPRESS_DEPRECATED)

# First-party warning bar (shared with the library; see cmake/Warnings.cmake).
include(Warnings)
pay_apply_warnings(${PROJECT_NAME})

# Opt-in gcov instrumentation (DROGON_PAY_COVERAGE); as the executable this
# is where the gcov runtime gets linked in.
include(Coverage)
pay_apply_gcov(${PROJECT_NAME})

# All platforms run the executable directly from its output directory so
# ./config.json and ./.env (copied POST_BUILD above) resolve. The single
# no-flag run executes every DROGON_TEST case in one process and the
# process exit code propagates to CTest unchanged.
add_test(
    NAME PayBackendTests
    COMMAND ${PROJECT_NAME}
    WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>
)
