cmake_minimum_required(VERSION 3.21)
project(PayBackendTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TEST_SRC
    CallbackControllerTest.cc     # P0: controller-level callback validation (C1-2, C2-1)
    ConfigLoaderTest.cc
    CreatePaymentIntegrationTest.cc  # [PASS] Updated to new API
    ControllerMetricsTest.cc
    HealthProbeTest.cc
    HttpResponseHeadersTest.cc    # P2: CORS preflight + security headers
    IdempotencyIntegrationTest.cc
    OnceCallbackTest.cc
    PayErrorCategoryTest.cc       # P3-7.2: PayErrorCategory singleton + concurrent
    QueryOrderListAndReconcileTest.cc  # P1: queryOrderList + reconcileSummary params
    test_main.cc
    AuthCheckTest.cc
    RouteRegistrationSmokeTest.cc
    PayAuthMetricsTest.cc
    PayUtilsTest.cc
    QueryOrderTest.cc
    WechatPayClientTest.cc
    WechatCallbackIntegrationTest.cc  # [PASS] Updated to new API (Phase 3)
    RefundQueryTest.cc  # [PASS] Updated to new API (Phase 3)
    ReconcileSummaryTest.cc  # [PASS] Updated to new API (Phase 3)
    StartupValidatorTest.cc
)

add_executable(${PROJECT_NAME} ${TEST_SRC})

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
    )
endif()

# First-party warning bar (shared with the library; see cmake/Warnings.cmake).
include(Warnings)
pay_apply_warnings(${PROJECT_NAME})

# Platform-specific test configuration
if(WIN32)
    # Windows: Use PowerShell script
    add_test(
        NAME PayBackendTests
        COMMAND powershell -ExecutionPolicy Bypass -File
                "${CMAKE_CURRENT_SOURCE_DIR}/run_all_tests.ps1"
                "$<TARGET_FILE:${PROJECT_NAME}>"
    )
else()
    # Linux/macOS: Run test executable directly
    add_test(
        NAME PayBackendTests
        COMMAND ${PROJECT_NAME}
        WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
endif()
