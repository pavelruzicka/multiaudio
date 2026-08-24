cmake_minimum_required(VERSION 3.15)
project(multiaudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_executable(multiaudio
        src/main.cpp
        src/mirror.cpp
        src/devices.cpp
        src/audio.cpp
        src/util.cpp
    )
    target_compile_definitions(multiaudio PRIVATE UNICODE _UNICODE)
    target_link_libraries(multiaudio PRIVATE ole32 avrt)

    if(MSVC)
        target_compile_options(multiaudio PRIVATE /W3)
    else()
        # MinGW: -municode selects the wmain entry point.
        target_compile_options(multiaudio PRIVATE -Wall -Wextra -municode)
        target_link_options(multiaudio PRIVATE -municode -static)
    endif()
else()
    message(STATUS "Not Windows: building the resampler test only.")
endif()

# The resampler and ring buffer are plain arithmetic and build anywhere.
enable_testing()
add_executable(resampler_test tests/resampler_test.cpp)
if(NOT MSVC)
    target_compile_options(resampler_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME resampler COMMAND resampler_test)
