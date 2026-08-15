# yaml-cpp 0.8.0 declares a 3.4 policy version. CMake 3.27 and newer
# deprecate that compatibility level; CMake 4 removes it and current releases
# warn below 3.10. This directory scope prevents the workaround from affecting
# Zeta or another dependency. FetchContent's internal scopes read these values
# from the cache, so preserve any caller setting around its population.
if(CMAKE_VERSION VERSION_LESS 4.0)
    set(_zeta_override_variable CMAKE_WARN_DEPRECATED)
    set(_zeta_override_value OFF)
    set(_zeta_override_type BOOL)
else()
    set(_zeta_override_variable CMAKE_POLICY_VERSION_MINIMUM)
    set(_zeta_override_value 3.10)
    set(_zeta_override_type STRING)
endif()

if(DEFINED CACHE{${_zeta_override_variable}})
    set(_zeta_had_override TRUE)
    set(_zeta_previous_value "$CACHE{${_zeta_override_variable}}")
endif()
set(${_zeta_override_variable} "${_zeta_override_value}")
set(
    ${_zeta_override_variable}
    "${_zeta_override_value}"
    CACHE ${_zeta_override_type} "" FORCE
)

FetchContent_MakeAvailable(yaml-cpp)

if(_zeta_had_override)
    set(
        ${_zeta_override_variable}
        "${_zeta_previous_value}"
        CACHE ${_zeta_override_type} "" FORCE
    )
else()
    unset(${_zeta_override_variable} CACHE)
endif()
