# Elevated warning bar for first-party targets.
#
# Implemented as a function (not an INTERFACE library) so the requirements
# attach PRIVATE and never leak into the install(EXPORT) consumer surface:
# `find_package(DrogonPay)` users must not inherit our /W4 -Wextra -Werror.
#
# DROGON_PAY_WERROR (root option, default OFF) flips the bar to
# warnings-as-errors; CI passes -DDROGON_PAY_WERROR=ON so the gate is real
# while local default builds stay friction-free.

include_guard(GLOBAL)

function(pay_apply_warnings target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "pay_apply_warnings: no such target '${target}'")
  endif()

  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra>
  )

  if(DROGON_PAY_WERROR)
    target_compile_options(${target} PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/WX>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>
    )
  endif()
endfunction()
