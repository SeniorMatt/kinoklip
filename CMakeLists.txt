cmake_minimum_required(VERSION 3.24)

# ---- Product identity (rename the app here + src/app/AppInfo.h) ----
set(APP_NAME "Cutwise")
set(APP_ID   "cutwise")

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

# FFmpeg: pkg-config on Linux/macOS. (Windows later: FFMPEG_ROOT + find_library.)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale libswresample libavfilter)

# ---- Core library: everything under src/ except main.cpp ----
file(GLOB_RECURSE CW_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h)
list(FILTER CW_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")

add_library(cutwise_core STATIC ${CW_SOURCES})
target_include_directories(cutwise_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(cutwise_core PUBLIC
  CW_APP_NAME="${APP_NAME}" CW_APP_ID="${APP_ID}" CW_APP_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(cutwise_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Multimedia Qt6::Svg Qt6::SvgWidgets
  Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Concurrent PkgConfig::FFMPEG)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cutwise_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Resources (icons, default keymap, ...): every resources/*.qrc is compiled into the core lib.
file(GLOB CW_QRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/resources/*.qrc)
if(CW_QRC)
  qt_add_resources(CW_QRC_OBJS ${CW_QRC})
  target_sources(cutwise_core PRIVATE ${CW_QRC_OBJS})
endif()

# ---- Application ----
add_executable(${APP_ID} src/main.cpp)
target_link_libraries(${APP_ID} PRIVATE cutwise_core)
set_target_properties(${APP_ID} PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
install(TARGETS ${APP_ID} RUNTIME DESTINATION bin BUNDLE DESTINATION .)

# ---- Tests: every tests/**/test_*.cpp becomes its own executable ----
enable_testing()
file(GLOB_RECURSE CW_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
foreach(test_src ${CW_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE cutwise_core Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE CW_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)
endforeach()
