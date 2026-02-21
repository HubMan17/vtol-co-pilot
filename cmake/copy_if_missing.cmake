# Copy file only if destination does not exist.
# Usage: cmake -Dsrc=<source> -Ddst=<destination> -P copy_if_missing.cmake
if(NOT EXISTS "${dst}")
    file(COPY_FILE "${src}" "${dst}")
    message(STATUS "Copied default config: ${dst}")
else()
    message(STATUS "Config exists, skipping: ${dst}")
endif()
