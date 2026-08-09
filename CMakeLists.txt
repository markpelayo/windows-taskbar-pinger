cmake_minimum_required(VERSION 3.20)

project(Pinger VERSION 1.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only.")
endif()

# A bare `cmake --build` with no config produces an unoptimised, no-NDEBUG
# binary, which is not what anyone typing two commands from the README expects.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(Pinger WIN32
    src/main.cpp
    src/app.cpp
    src/monitor.cpp
    src/grid.cpp
    src/ping.cpp
    src/taskbar.cpp
    src/settings.cpp
    src/dialogs.cpp
    src/autostart.cpp
    src/resources.rc
    src/app.manifest
)

target_include_directories(Pinger PRIVATE src)

target_compile_definitions(Pinger PRIVATE
    UNICODE
    _UNICODE
    # Trims rarely used declarations out of windows.h, which cuts compile time
    # noticeably on a project this size.
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(Pinger PRIVATE
    iphlpapi      # IcmpCreateFile, IcmpSendEcho
    ws2_32        # GetAddrInfoW
    shell32       # Shell_NotifyIcon, ShellExecute, SHCreateDirectoryEx
    ole32         # CoCreateGuid, CoTaskMemFree
    uuid          # the FOLDERID_RoamingAppData GUID itself
    comdlg32      # ChooseColor
    gdi32         # the drawing
    user32
    advapi32
)

if(MSVC)
    # CMake puts /W3 in the default flags, so appending /W4 below would make the
    # compiler warn about the override on every file.
    string(REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

    # Static CRT, matching build.bat.
    #
    # Without this CMake defaults to /MD, and the resulting binary refuses to
    # start on any machine without the Visual C++ redistributable installed —
    # which quietly contradicted both the README's "nothing else to run it" and
    # the release notes. The shipped artifact was always fine because CI runs
    # build.bat, but anyone building through CMake got a materially more fragile
    # executable.
    set_property(TARGET Pinger PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # /utf-8 matters: the menus contain typographic characters (·, …, curly
    # quotes, an em dash) and without it MSVC reads these sources as the system
    # code page and mangles them.
    target_compile_options(Pinger PRIVATE /W4 /permissive- /utf-8)

    # Release favours size: this is a widget, not a hot loop. /O1 already implies
    # /Os and /Gy. /Gw lets /OPT:REF discard unreferenced data, not just code.
    target_compile_options(Pinger PRIVATE $<$<CONFIG:Release>:/O1 /Gw /Zc:inline /GL>)
    target_link_options(Pinger PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)

    # Subsystem is set by WIN32 above; this names the correct entry point for
    # the wide-character wWinMain.
    set_target_properties(Pinger PROPERTIES LINK_FLAGS "/ENTRY:wWinMainCRTStartup")
endif()
