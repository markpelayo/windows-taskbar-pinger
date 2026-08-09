cmake_minimum_required(VERSION 3.20)

project(Pinger VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only.")
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
    # /utf-8 matters: the menus contain typographic characters (·, …, curly
    # quotes, an em dash) and without it MSVC reads these sources as the system
    # code page and mangles them.
    target_compile_options(Pinger PRIVATE /W4 /permissive- /utf-8)

    # Release builds favour size: this is a widget, not a hot loop.
    target_compile_options(Pinger PRIVATE $<$<CONFIG:Release>:/O1 /GL>)
    target_link_options(Pinger PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)

    # Subsystem is set by WIN32 above; this names the correct entry point for
    # the wide-character wWinMain.
    set_target_properties(Pinger PROPERTIES LINK_FLAGS "/ENTRY:wWinMainCRTStartup")
endif()
