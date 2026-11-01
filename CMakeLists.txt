cmake_minimum_required(VERSION 3.24)

# ---- Product identity (rename the app here + src/app/AppInfo.h) ----
set(APP_NAME "Kinoklip")
set(APP_ID   "kinoklip")

project(${APP_NAME} VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Multimedia Svg SvgWidgets OpenGL OpenGLWidgets Concurrent Test)

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
  Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Concurrent PkgConfig::FFMPEG)
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
file(GLOB_RECURSE KK_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
foreach(test_src ${KK_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE kinoklip_core Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE KK_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)
endforeach()
