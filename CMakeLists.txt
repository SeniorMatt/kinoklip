cmake_minimum_required(VERSION 3.24)

# ---- Product identity (rename the app here + src/app/AppInfo.h) ----
set(APP_NAME "Kinoklip")
set(APP_ID   "kinoklip")

project(${APP_NAME} VERSION 1.1.8 LANGUAGES CXX)

# Reuse compiled files between build folders and worktrees when ccache is installed.
find_program(KK_CCACHE ccache)
if(KK_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${KK_CCACHE})
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Multimedia Svg SvgWidgets OpenGL OpenGLWidgets Concurrent DBus Test)

# Qt's private QDesktopUnixServices (Qt >= 6.9) exports our window for the XDG desktop portal
# ("wayland:<handle>"), so the native file dialog is attached to the Kinoklip window
# (src/platform/PortalFileChooser.cpp). Optional: without it the dialog still works, unparented
# on Wayland.
set(KK_HAVE_PORTAL_WINDOW_ID OFF)
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.9)
  find_package(Qt6 QUIET COMPONENTS GuiPrivate)
  if(TARGET Qt6::GuiPrivate)
    set(KK_HAVE_PORTAL_WINDOW_ID ON)
  endif()
endif()
message(STATUS "Portal parent-window export (Qt private API): ${KK_HAVE_PORTAL_WINDOW_ID}")

# KDE Frameworks 6 (optional): the real Breeze look the Kdenlive way -- KColorSchemeManager for
# the KDE colour schemes, KIconThemes (+ the Breeze icons compiled into libKF6BreezeIcons) for
# the icon theme. Present in the Arch container build; the Qt 6.4 fallback build (Mint) has no
# KF6 and uses palette approximations read from resources/color-schemes (src/app/Theme.cpp).
option(KK_USE_KF6 "Use KDE Frameworks 6 (KColorScheme, KIconThemes) when available" ON)
set(KK_HAVE_KF6 OFF)
if(KK_USE_KF6)
  find_package(KF6ColorScheme 6.3 QUIET)
  find_package(KF6IconThemes 6.3 QUIET)
  if(KF6ColorScheme_FOUND AND KF6IconThemes_FOUND)
    set(KK_HAVE_KF6 ON)
  endif()
endif()
message(STATUS "KDE Frameworks 6 (Breeze colour schemes + icons): ${KK_HAVE_KF6}")

# FFmpeg: pkg-config on Linux/macOS. (Windows later: FFMPEG_ROOT + find_library.)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale libswresample libavfilter)

# Optional still-image libraries (src/media/ImageLoader): HEIC/HEIF/AVIF and camera RAW.
# Without them those formats are reported unsupported (AVIF still opens through FFmpeg).
option(KK_WITH_LIBHEIF "HEIC/HEIF/AVIF stills via libheif (if found)" ON)
option(KK_WITH_LIBRAW "Camera RAW stills via LibRaw (if found)" ON)
if(KK_WITH_LIBHEIF)
  pkg_check_modules(LIBHEIF IMPORTED_TARGET libheif)
endif()
if(KK_WITH_LIBRAW)
  pkg_check_modules(LIBRAW IMPORTED_TARGET libraw_r) # thread-safe LibRaw build
endif()
message(STATUS "libheif: ${LIBHEIF_FOUND} ${LIBHEIF_VERSION}, LibRaw: ${LIBRAW_FOUND} ${LIBRAW_VERSION}")

# ---- Core library: everything under src/ except main.cpp ----
file(GLOB_RECURSE KK_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h)
list(FILTER KK_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")

add_library(kinoklip_core STATIC ${KK_SOURCES})
target_include_directories(kinoklip_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(kinoklip_core PUBLIC
  KK_APP_NAME="${APP_NAME}" KK_APP_ID="${APP_ID}" KK_APP_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(kinoklip_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Multimedia Qt6::Svg Qt6::SvgWidgets
  Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Concurrent Qt6::DBus PkgConfig::FFMPEG)
if(KK_HAVE_PORTAL_WINDOW_ID)
  target_link_libraries(kinoklip_core PRIVATE Qt6::GuiPrivate)
  target_compile_definitions(kinoklip_core PRIVATE KK_HAVE_PORTAL_WINDOW_ID)
endif()
if(KK_HAVE_KF6)
  target_link_libraries(kinoklip_core PUBLIC KF6::ColorScheme KF6::IconThemes)
  target_compile_definitions(kinoklip_core PUBLIC KK_HAVE_KF6)
endif()
if(LIBHEIF_FOUND)
  target_link_libraries(kinoklip_core PUBLIC PkgConfig::LIBHEIF)
  target_compile_definitions(kinoklip_core PUBLIC KK_HAVE_LIBHEIF=1)
endif()
if(LIBRAW_FOUND)
  target_link_libraries(kinoklip_core PUBLIC PkgConfig::LIBRAW)
  target_compile_definitions(kinoklip_core PUBLIC KK_HAVE_LIBRAW=1)
endif()
# The commit a build comes from, for Help > Report a Bug (read at configure time; set on that
# one file so a new commit does not rebuild the library). Empty outside a git checkout.
find_package(Git QUIET)
set(KK_GIT_COMMIT "")
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=10 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE KK_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/app/BugReport.cpp
  PROPERTIES COMPILE_DEFINITIONS "KK_GIT_COMMIT=\"${KK_GIT_COMMIT}\"")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(kinoklip_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Resources (icons, default keymap, ...): every resources/*.qrc is compiled into the core lib.
file(GLOB KK_QRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/resources/*.qrc)
if(KK_QRC)
  qt_add_resources(KK_QRC_OBJS ${KK_QRC})
  target_sources(kinoklip_core PRIVATE ${KK_QRC_OBJS})
endif()

# ---- Application ----
add_executable(${APP_ID} src/main.cpp)
target_link_libraries(${APP_ID} PRIVATE kinoklip_core)
set_target_properties(${APP_ID} PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
install(TARGETS ${APP_ID} RUNTIME DESTINATION bin BUNDLE DESTINATION .)

# ---- Tests: every tests/**/test_*.cpp becomes its own executable ----
enable_testing()
find_program(KK_DBUS_RUN_SESSION dbus-run-session)
file(GLOB_RECURSE KK_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
foreach(test_src ${KK_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE kinoklip_core Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE KK_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
  if(test_name MATCHES "^test_(filedialogs|rendernotify|openurl)$" AND KK_DBUS_RUN_SESSION)
    # their fake portal / notification / OpenURI services need a private session bus (skipped without)
    add_test(NAME ${test_name} COMMAND ${KK_DBUS_RUN_SESSION} -- $<TARGET_FILE:${test_name}>)
  else()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endif()
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)
endforeach()

# test_cli runs the built application as a process.
if(TARGET test_cli)
  target_compile_definitions(test_cli PRIVATE KK_BINARY_PATH="$<TARGET_FILE:${APP_ID}>")
  add_dependencies(test_cli ${APP_ID})
endif()
# test_livecontrol starts the built application as `kinoklip mcp` / `kinoklip <command> --live`.
if(TARGET test_livecontrol)
  target_compile_definitions(test_livecontrol PRIVATE KK_BINARY_PATH="$<TARGET_FILE:${APP_ID}>")
  add_dependencies(test_livecontrol ${APP_ID})
endif()

# The GitHub issue forms (.github/ISSUE_TEMPLATE) and the field ids Help > Report a Bug fills.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME check_issue_forms
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_issue_forms.py)
endif()

# test_wayland once more under a real compositor with real input (weston on Xvfb, xdotool):
# drags through wl_data_device, no explicit grabs, floating panels. Only where the tools exist
# (the Arch container); offscreen the test skips itself.
find_program(KK_WESTON weston)
find_program(KK_XVFB Xvfb)
find_program(KK_XDOTOOL xdotool)
if(KK_WESTON AND KK_XVFB AND KK_XDOTOOL)
  add_test(NAME test_wayland_session
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/wayland_session.sh $<TARGET_FILE:test_wayland>)
  set_tests_properties(test_wayland_session PROPERTIES
    ENVIRONMENT "KK_NO_AUDIO=1;KK_XDISPLAY=:88;KK_WESTON_LOG=${CMAKE_BINARY_DIR}/weston-test.log"
    TIMEOUT 300 RUN_SERIAL TRUE)
endif()

# Helper for the nested Wayland session checks (scripts/wayland_dnd_check.sh): an external drag
# source standing in for Dolphin. Not a test itself.
add_executable(kk_dragsource tests/tools/kk_dragsource.cpp)
target_link_libraries(kk_dragsource PRIVATE Qt6::Widgets)

# Input injector for the KWin variant of those checks (KWin's org_kde_kwin_fake_input
# protocol). Optional: needs wayland-client, wayland-scanner and plasma-wayland-protocols
# (present in the Arch container only).
find_program(KK_WAYLAND_SCANNER wayland-scanner)
pkg_check_modules(KK_WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
find_file(KK_FAKE_INPUT_XML fake-input.xml PATHS /usr/share/plasma-wayland-protocols NO_DEFAULT_PATH)
if(KK_WAYLAND_SCANNER AND KK_WAYLAND_CLIENT_FOUND AND KK_FAKE_INPUT_XML)
  enable_language(C)
  set(_kk_fi ${CMAKE_BINARY_DIR}/kk_fakeinput_gen)
  file(MAKE_DIRECTORY ${_kk_fi})
  add_custom_command(
    OUTPUT ${_kk_fi}/fake-input-client-protocol.h ${_kk_fi}/fake-input-protocol.c
    COMMAND ${KK_WAYLAND_SCANNER} client-header ${KK_FAKE_INPUT_XML} ${_kk_fi}/fake-input-client-protocol.h
    COMMAND ${KK_WAYLAND_SCANNER} private-code ${KK_FAKE_INPUT_XML} ${_kk_fi}/fake-input-protocol.c
    DEPENDS ${KK_FAKE_INPUT_XML})
  add_executable(kk_fakeinput tests/tools/kk_fakeinput.c ${_kk_fi}/fake-input-protocol.c
    ${_kk_fi}/fake-input-client-protocol.h)
  target_include_directories(kk_fakeinput PRIVATE ${_kk_fi})
  target_link_libraries(kk_fakeinput PRIVATE PkgConfig::KK_WAYLAND_CLIENT)
endif()

# Writes a project with imported media for those checks (media pool items to drag).
add_executable(kk_makeproject tests/tools/kk_makeproject.cpp)
target_link_libraries(kk_makeproject PRIVATE kinoklip_core)

# Contact sheets of the video transitions (tests/tools/kk_transitionsheets.cpp), to judge their
# look by eye; a test tool, not installed. Run under xvfb-run with QT_QPA_PLATFORM=xcb.
add_executable(kk_transitionsheets tests/tools/kk_transitionsheets.cpp)
target_link_libraries(kk_transitionsheets PRIVATE kinoklip_core)
