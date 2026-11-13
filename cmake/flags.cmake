include(CMakeCheckCompilerFlagCommonPatterns)
check_compiler_flag_common_patterns(flag_fail_patterns)

set(gnu_strict -Werror)
set(gnu_strict_link -Wl,--fatal-warnings)
set(gnu_common -Wall -Wextra -pedantic -Wshadow)
set(gnu_debug
    -O0
    -ggdb3
    -fdebug-macro
    -fno-builtin
    -fno-inline
    -fno-omit-frame-pointer
    -mno-omit-leaf-frame-pointer
    -fno-optimize-sibling-calls
    -fasynchronous-unwind-tables
    -fno-stack-protector
    -fno-stack-clash-protection
    -fcf-protection=none
    -mbranch-protection=none
    -U_FORTIFY_SOURCE
    -ffp-contract=off
    -mno-relax
    -fno-eliminate-unused-debug-types
    -fkeep-inline-functions
    -fkeep-static-functions
    -fkeep-persistent-storage-variables
    -grecord-gcc-switches
    -fno-pie
)
set(gnu_debug_link -no-pie -rdynamic -Wl,--build-id)
set(gnu_profile -pg)
set(gnu_release
    -O2
    -g
    -fasynchronous-unwind-tables
    -fstack-protector-strong
    -fstack-clash-protection
    -fno-omit-frame-pointer
    -mno-omit-leaf-frame-pointer
    -Wformat
    -Werror=format-security
    -fcf-protection=full
    -mbranch-protection=standard
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=3
    -fPIE
)
set(gnu_release_link
    -pie
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,noexecstack
    -Wl,-z,pack-relative-relocs
    -Wl,--as-needed
    -Wl,--build-id
)
set(gnu_optimized -O2)

set(msvc_strict /WX)
set(msvc_strict_link "")
set(msvc_common /utf-8 /W4 /D_CRT_SECURE_NO_WARNINGS)
set(msvc_debug /Od /Ob0 /Zi /GS-)
set(msvc_debug_link /DEBUG /INCREMENTAL:NO)
set(msvc_release /O2 /Zi /sdl /GS /guard:cf)
set(msvc_release_link /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF /guard:cf /CETCOMPAT)
set(msvc_optimized /O2)
set(msvc_profile "")

option(ENABLE_LTO "Link-time optimization in Release" ON)
set(SANITIZE address CACHE STRING "Sanitizers of the Sanitize config, e.g. address,undefined")
if(SANITIZE STREQUAL "")
    set(SANITIZE address CACHE STRING "Sanitizers of the Sanitize config, e.g. address,undefined" FORCE)
endif()
set(gnu_sanitize ${gnu_debug} -fsanitize=${SANITIZE} -fno-sanitize-recover=all)
set(gnu_sanitize_link ${gnu_debug_link} -fsanitize=${SANITIZE})
set(msvc_sanitize ${msvc_debug} /fsanitize=${SANITIZE})
set(msvc_sanitize_link ${msvc_debug_link})

function(probe_flags out config)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "LINK" "" "CONTEXT;LINK_CONTEXT;FLAGS")
    # try_compile forwards CMAKE_C_FLAGS_<config>; probe in that config with the defaults cleared
    set(try_config ${config})
    if(NOT config MATCHES "^(Debug|Release)$")
        set(try_config Debug)
    endif()
    string(TOUPPER ${try_config} config_upper)
    set(CMAKE_TRY_COMPILE_CONFIGURATION ${try_config})
    set(CMAKE_C_FLAGS_${config_upper} "")
    set(accepted "")
    foreach(flag IN LISTS ARG_FLAGS)
        list(JOIN ARG_CONTEXT " " context)
        list(JOIN accepted " " prefix)
        list(JOIN ARG_LINK_CONTEXT " " link_context)
        string(MD5 context_id
               "${config} ${flag} ${${FLAG_SYNTAX}_strict} ${${FLAG_SYNTAX}_strict_link} ${context} ${link_context} ${prefix}")
        string(SUBSTRING ${context_id} 0 8 context_id)
        if(ARG_LINK)
            string(MAKE_C_IDENTIFIER "LINKS_${flag}_${context_id}" probe)
            set(CMAKE_REQUIRED_LINK_OPTIONS ${${FLAG_SYNTAX}_strict_link} ${ARG_CONTEXT} ${accepted} ${flag})
        else()
            string(MAKE_C_IDENTIFIER "ACCEPTS_${flag}_${context_id}" probe)
            set(CMAKE_REQUIRED_FLAGS "${${FLAG_SYNTAX}_strict} ${context} ${prefix} ${flag}")
            set(CMAKE_REQUIRED_LINK_OPTIONS ${ARG_LINK_CONTEXT})
        endif()
        check_c_source_compiles("int main(void) { return 0; }" ${probe} ${flag_fail_patterns})
        if(${probe})
            list(APPEND accepted "${flag}")
        endif()
    endforeach()
    set(${out} "${accepted}" PARENT_SCOPE)
endfunction()

function(report label)
    list(JOIN ARGN " " text)
    message(STATUS ">> ${label}: ${text}")
endfunction()

set(common ${${FLAG_SYNTAX}_common})
set(OPTIMIZED_FLAGS ${${FLAG_SYNTAX}_optimized})
foreach(config IN LISTS BUILD_CONFIGS)
    string(TOLOWER ${config} table)
    string(TOUPPER ${config} config_upper)
    if(NOT DEFINED ${FLAG_SYNTAX}_${table} OR NOT DEFINED ${FLAG_SYNTAX}_${table}_link)
        message(FATAL_ERROR "No ${FLAG_SYNTAX} flag table for ${config}.")
    endif()
    foreach(variable CMAKE_C_FLAGS_${config_upper} CMAKE_EXE_LINKER_FLAGS_${config_upper})
        string(STRIP "$CACHE{${variable}}" cached)
        string(STRIP "${${variable}_INIT}" initial)
        if(NOT cached STREQUAL initial)
            message(WARNING "${variable} is ignored: flags come from cmake/flags.cmake, extra ones go in CMAKE_C_FLAGS.")
        endif()
    endforeach()
    probe_flags(common ${config} FLAGS ${common})
    probe_flags(compile ${config} CONTEXT ${common} FLAGS ${${FLAG_SYNTAX}_${table}})
    probe_flags(link ${config} LINK FLAGS ${${FLAG_SYNTAX}_${table}_link})
    probe_flags(OPTIMIZED_FLAGS ${config} CONTEXT ${common} ${compile} LINK_CONTEXT ${link} FLAGS ${OPTIMIZED_FLAGS})
    if(table STREQUAL "debug")
        probe_flags(PROFILE_FLAGS ${config} CONTEXT ${common} ${compile} LINK_CONTEXT ${link}
                    FLAGS ${${FLAG_SYNTAX}_profile})
    endif()
    if(table STREQUAL "sanitize")
        set(sanitize_ok FALSE)
        foreach(flag IN LISTS compile)
            if(flag MATCHES "^[-/]fsanitize=")
                set(sanitize_ok TRUE)
            endif()
        endforeach()
        set(SANITIZE_OK ${sanitize_ok} CACHE INTERNAL "")
    endif()
    list(JOIN compile " " CMAKE_C_FLAGS_${config_upper})
    list(JOIN link " " CMAKE_EXE_LINKER_FLAGS_${config_upper})
    set(dropped ${${FLAG_SYNTAX}_${table}} ${${FLAG_SYNTAX}_${table}_link})
    foreach(flag IN LISTS compile link)
        list(REMOVE_ITEM dropped "${flag}")
    endforeach()
    report("${config}" ${compile})
    report("${config} link" ${link})
    if(dropped)
        report("${config} dropped" ${dropped})
    endif()
endforeach()
add_compile_options(${common})
add_compile_definitions($<$<CONFIG:Release>:NDEBUG>)
report("Every config" ${common})
report("Debug and Sanitize, unless UNPROFILED" ${PROFILE_FLAGS})
if(SANITIZE_OK)
    report("Sanitize" "links -fsanitize=${SANITIZE}")
else()
    report("Sanitize" "${CMAKE_C_COMPILER} cannot link -fsanitize=${SANITIZE}; build.cmake skips this config")
endif()
set(dropped ${${FLAG_SYNTAX}_common} ${${FLAG_SYNTAX}_optimized})
foreach(flag IN LISTS common OPTIMIZED_FLAGS)
    list(REMOVE_ITEM dropped "${flag}")
endforeach()
if(dropped)
    report("Every config dropped" ${dropped})
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
if(ENABLE_LTO)
    if(NOT DEFINED CACHE{LTO_SUPPORTED})
        include(CheckIPOSupported)
        check_ipo_supported(RESULT supported LANGUAGES C)
        set(LTO_SUPPORTED ${supported} CACHE INTERNAL "")
    endif()
    if(LTO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(WARNING "Release builds without LTO: ${CMAKE_C_COMPILER_ID} fails check_ipo_supported.")
    endif()
endif()
report("Release LTO" ${CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE})