cmake_minimum_required(VERSION 3.15)
project(multiaudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    # WIN32 = no console window; the command line options borrow the console
    # they were started from.
    add_executable(multiaudio WIN32
        src/main.cpp
        src/tray.cpp
        src/mirror.cpp
        src/devices.cpp
        src/audio.cpp
        src/settings.cpp
        src/install.cpp
        src/util.cpp
    )
    target_compile_definitions(multiaudio PRIVATE UNICODE _UNICODE)
    target_link_libraries(multiaudio PRIVATE ole32 avrt shell32 user32 gdi32 advapi32)

    if(MSVC)
        target_compile_definitions(multiaudio PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(multiaudio PRIVATE /W3)
    else()
        # MinGW: -municode selects the wide entry point.
        target_compile_options(multiaudio PRIVATE -Wall -Wextra -municode)
        target_link_options(multiaudio PRIVATE -municode -static)
    endif()
else()
    message(STATUS "Not Windows: building the audio test only.")
endif()

# The resampler, channel map and ring buffer are plain arithmetic: they build
# and run anywhere, including on the machine that cross-compiles the exe.
enable_testing()
add_executable(audio_test tests/audio_test.cpp)
if(NOT MSVC)
    target_compile_options(audio_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME audio COMMAND audio_test)
