# Usage:
#   cmake -Dsrc=<source-file> -Ddst=<destination-file> -P safe_copy_if_different.cmake
#
# This helper retries copy operations to tolerate transient DLL locks on Windows.
# If the destination stays locked, it logs a warning and returns success so build
# artifacts are still produced under the build tree.

if(NOT DEFINED src OR NOT DEFINED dst)
    message(FATAL_ERROR "safe_copy_if_different.cmake requires -Dsrc and -Ddst")
endif()

get_filename_component(dst_dir "${dst}" DIRECTORY)
file(MAKE_DIRECTORY "${dst_dir}")

set(max_attempts 20)
set(delay_seconds 0.25)
set(copy_ok FALSE)

foreach(attempt RANGE 1 ${max_attempts})
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${src}" "${dst}"
        RESULT_VARIABLE copy_result
        OUTPUT_QUIET
        ERROR_QUIET
    )

    if(copy_result EQUAL 0)
        set(copy_ok TRUE)
        break()
    endif()

    if(attempt LESS max_attempts)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep "${delay_seconds}")
    endif()
endforeach()

if(NOT copy_ok)
    message(WARNING
        "Could not deploy plugin after ${max_attempts} attempts: ${src} -> ${dst}. "
        "Destination may be locked by another process. Build continues.")
endif()
