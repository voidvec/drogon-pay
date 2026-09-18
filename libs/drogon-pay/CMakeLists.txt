# ##############################################################################
# drogon-pay: the reusable Drogon payment plugin library (services, channels,
# ORM models, utils, HTTP handlers). Routes are registered programmatically by
# PayPlugin (no ADD_METHOD_TO), so the library is safe to consume as a STATIC
# library without symbol-drop concerns. Exported as DrogonPay::DrogonPay via
# find_package(DrogonPay).
# ##############################################################################
add_library(drogon-pay STATIC
    src/ChannelRegistry.cc
    src/PayPlugin.cc
    src/channels/AlipayChannel.cc
    src/channels/WechatChannel.cc
    src/handlers/AuthCheck.cc
    src/handlers/CallbackHandlers.cc
    src/handlers/PayAuthMetrics.cc
    src/handlers/PayHandlers.cc
    src/handlers/PayMetricsHandlers.cc
    src/services/CallbackService.cc
    src/services/IdempotencyService.cc
    src/services/PaymentService.cc
    src/services/ReconciliationService.cc
    src/services/RefundService.cc
    src/utils/PayUtils.cc
)

# ------------------------------------------------------------------------------
# Generated ORM models live in their own OBJECT library on purpose: they are
# drogon_ctl output and must never be hand-edited, so they compile with the
# advisory /W4 (-Wall -Wextra) profile only, WITHOUT the WERROR bar that the
# first-party drogon-pay target carries under DROGON_PAY_WERROR.
# ------------------------------------------------------------------------------
add_library(drogon_pay_models OBJECT
    src/models/PayCallback.cc
    src/models/PayIdempotency.cc
    src/models/PayLedger.cc
    src/models/PayOrder.cc
    src/models/PayPayment.cc
    src/models/PayRefund.cc
)

target_include_directories(drogon_pay_models PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/models
)
target_link_libraries(drogon_pay_models PRIVATE Drogon::Drogon OpenSSL::Crypto OpenSSL::SSL)
if(MSVC)
    target_compile_options(drogon_pay_models PRIVATE /FI"orm_compat.h" /utf-8)
    target_compile_definitions(drogon_pay_models PRIVATE
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(drogon_pay_models PRIVATE -include orm_compat.h)
    target_compile_definitions(drogon_pay_models PRIVATE
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    )
endif()
target_compile_options(drogon_pay_models PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

# Alias kept for in-tree consumers (example host, tests) that predate the
# formal library name.
add_library(pay_core ALIAS drogon-pay)
add_library(DrogonPay::DrogonPay ALIAS drogon-pay)

# include/  : public API surface (drogon_pay/*.h) — the only headers hosts see
#             and the only headers installed.
# src/      : internal headers; BUILD_INTERFACE-only so the white-box test
#             suite can include services/channels/handlers directly, while
#             installed consumers never see them.
target_include_directories(drogon-pay PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(drogon-pay PUBLIC Drogon::Drogon OpenSSL::Crypto OpenSSL::SSL)

# OBJECT-library linking pulls the model object files into the drogon-pay
# archive. $<TARGET_OBJECTS> is used instead of target_link_libraries so the
# private helper target does not have to join install(EXPORT).
target_sources(drogon-pay PRIVATE $<TARGET_OBJECTS:drogon_pay_models>)

# Force-include orm_compat.h for the first-party sources too: they include the
# generated model headers, which transitively pull the deprecated codecvt
# surface. This provides the codecvt_utf8_utf16 fallback for C++20 and
# MSVC >= 19.43. (The generated model .cc files carry the same flag in the
# drogon_pay_models OBJECT library above.)
target_include_directories(drogon-pay PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/models>
)
if(MSVC)
    target_compile_options(drogon-pay PRIVATE /FI"orm_compat.h" /utf-8)
    target_compile_definitions(drogon-pay PUBLIC
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(drogon-pay PRIVATE -include orm_compat.h)
    target_compile_definitions(drogon-pay PUBLIC
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    )
endif()

# Elevated warning bar for first-party targets (see cmake/Warnings.cmake;
# PRIVATE semantics keep it off the installed consumer surface).
include(Warnings)
pay_apply_warnings(drogon-pay)

# Opt-in gcov instrumentation (DROGON_PAY_COVERAGE). drogon_pay_models is
# deliberately not instrumented — generated code is excluded from the
# coverage metric anyway (see scripts/measure_coverage.py).
include(Coverage)
pay_apply_gcov(drogon-pay)

# ------------------------------------------------------------------------------
# Install & export: find_package(DrogonPay) -> DrogonPay::DrogonPay
# ------------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS drogon-pay
    EXPORT DrogonPayTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/drogon_pay
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT DrogonPayTargets
    FILE DrogonPayTargets.cmake
    NAMESPACE DrogonPay::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DrogonPay
)

configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/DrogonPayConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/DrogonPayConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DrogonPay
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/DrogonPayConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/DrogonPayConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/DrogonPayConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DrogonPay
)
