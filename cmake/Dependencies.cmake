# Dependencies.cmake
# Uses find_package (vcpkg) if available, falls back to FetchContent

# spdlog
find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
    include(FetchContent)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.14.1
        GIT_SHALLOW    TRUE
    )
    set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(spdlog)
    message(STATUS "spdlog: FetchContent")
else()
    message(STATUS "spdlog: found via find_package")
endif()

# nlohmann/json
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
    message(STATUS "nlohmann_json: FetchContent")
else()
    message(STATUS "nlohmann_json: found via find_package")
endif()
