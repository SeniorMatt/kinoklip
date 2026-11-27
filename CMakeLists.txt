cmake_minimum_required(VERSION 3.24)

# ---- Product identity (rename the app here + src/app/AppInfo.h) ----
set(APP_NAME "Kinoklip")
set(APP_ID   "kinoklip")

project(${APP_NAME} VERSION 1.15.0 LANGUAGES C CXX)

# mold forks a helper by default that nobody reaps in a container started without an init
# process; thousands of zombies then use up the container's process limit. Link in one process.
if(CMAKE_LINKER_TYPE STREQUAL "MOLD")
    add_link_options(-Wl,--no-fork)
endif()

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

# Sanitizers (docs/contributing.md, "Memory leaks"): -DKK_SANITIZE=address or address,undefined
# adds -fsanitize to the application, the library and every test. Use a separate build folder
# (build-asan); scripts/check_leaks.sh configures and runs it.
set(KK_SANITIZE "" CACHE STRING "Sanitizers to build with: address, undefined or address,undefined")
if(KK_SANITIZE)
  add_compile_options(-fsanitize=${KK_SANITIZE} -fno-omit-frame-pointer -fno-optimize-sibling-calls)
  add_link_options(-fsanitize=${KK_SANITIZE})
  add_compile_definitions(KK_SANITIZED=1) # timing tests skip their speed asserts (about 3x slower)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Multimedia Svg SvgWidgets OpenGL OpenGLWidgets Concurrent DBus Network Test)

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
# Optional LV2 hosting for audio effects (src/audioplug/Lv2Host.cpp, spec 159): lilv (ISC) with serd, sord,
# sratom, zix and the LV2 headers (all ISC / MIT-style). Without lilv the Plugins page says LV2 is not
# available in this build; CLAP (vendored MIT headers) needs nothing.
option(KK_WITH_LV2 "Host LV2 audio effect plugins through lilv (if found)" ON)
if(KK_WITH_LV2)
  pkg_check_modules(LILV IMPORTED_TARGET lilv-0 lv2)
endif()
message(STATUS "LV2 hosting (lilv): ${LILV_FOUND} ${LILV_VERSION}")
# Optional AI runtime for Background Removal (src/matting): ONNX Runtime (MIT), CPU. Without it the
# tool is built but reports itself unavailable (spec 78).
option(KK_WITH_ONNXRUNTIME "Background Removal: run the model with ONNX Runtime (if found)" ON)
if(KK_WITH_ONNXRUNTIME)
  find_path(KK_ORT_INCLUDE onnxruntime_cxx_api.h PATH_SUFFIXES onnxruntime onnxruntime/core/session)
  find_library(KK_ORT_LIBRARY onnxruntime)
endif()
message(STATUS "ONNX Runtime: ${KK_ORT_LIBRARY}")
message(STATUS "libheif: ${LIBHEIF_FOUND} ${LIBHEIF_VERSION}, LibRaw: ${LIBRAW_FOUND} ${LIBRAW_VERSION}")

# ---- Core library: everything under src/ except main.cpp ----
file(GLOB_RECURSE KK_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h)
list(FILTER KK_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")
# The OpenFX host is its own small library (below) so the probe program needs no Qt Widgets.
list(FILTER KK_SOURCES EXCLUDE REGEX ".*/src/ofx/.*")
list(FILTER KK_SOURCES EXCLUDE REGEX ".*/src/audioplug/.*")

add_library(kinoklip_core STATIC ${KK_SOURCES})

# OpenFX host (spec 158): headers in third_party/openfx (BSD-3-Clause); Qt Core and dlopen only.
add_library(kk_ofx STATIC src/ofx/OfxHost.cpp src/ofx/OfxInfo.cpp src/ofx/OfxHost.h)
target_include_directories(kk_ofx PUBLIC ${CMAKE_SOURCE_DIR}/src
  PRIVATE ${CMAKE_SOURCE_DIR}/third_party/openfx/include)
target_link_libraries(kk_ofx PUBLIC Qt6::Core ${CMAKE_DL_LIBS})
set_target_properties(kk_ofx PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(kinoklip_core PUBLIC kk_ofx)
# kinoklip-ofx-probe: loads one OpenFX bundle in a process of its own (bad-plugin detection).
add_executable(kinoklip-ofx-probe src/ofx/probe_main.cpp)
target_link_libraries(kinoklip-ofx-probe PRIVATE kk_ofx)
install(TARGETS kinoklip-ofx-probe RUNTIME DESTINATION bin)

# kinoklip-hwenc-probe: opens one hardware encoder in a process of its own (a crashing driver, issue 18).
add_executable(kinoklip-hwenc-probe src/export/hwenc_probe_main.cpp)
target_link_libraries(kinoklip-hwenc-probe PRIVATE kinoklip_core)
install(TARGETS kinoklip-hwenc-probe RUNTIME DESTINATION bin)

# LV2 / CLAP audio effect host (spec 159): Qt Core, dlopen, lilv when found; CLAP headers in third_party/clap (MIT).
add_library(kk_audioplug STATIC
  src/audioplug/AudioPluginInfo.cpp src/audioplug/AudioPluginHost.cpp src/audioplug/Lv2Host.cpp
  src/audioplug/ClapHost.cpp src/audioplug/AudioPluginInfo.h src/audioplug/AudioPluginHost.h
  src/audioplug/AudioPluginCore.h src/audioplug/Lv2Host.h src/audioplug/ClapHost.h)
target_include_directories(kk_audioplug PUBLIC ${CMAKE_SOURCE_DIR}/src
  PRIVATE ${CMAKE_SOURCE_DIR}/third_party/clap/include)
target_link_libraries(kk_audioplug PUBLIC Qt6::Core ${CMAKE_DL_LIBS})
set_target_properties(kk_audioplug PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(LILV_FOUND)
  target_link_libraries(kk_audioplug PUBLIC PkgConfig::LILV)
  target_compile_definitions(kk_audioplug PRIVATE KK_HAVE_LV2=1)
endif()
target_link_libraries(kinoklip_core PUBLIC kk_audioplug)
# kinoklip-audio-probe: loads one LV2 bundle or CLAP file in a process of its own (bad-plugin detection).
add_executable(kinoklip-audio-probe src/audioplug/probe_main.cpp)
target_link_libraries(kinoklip-audio-probe PRIVATE kk_audioplug)
install(TARGETS kinoklip-audio-probe RUNTIME DESTINATION bin)

# RNNoise (BSD-3-Clause, third_party/rnnoise/readme.md): the neural noise suppressor of the Noise
# Reduction audio effect. Plain C, unchanged, built with warnings off.
add_library(kk_rnnoise STATIC
  third_party/rnnoise/src/denoise.c third_party/rnnoise/src/celt_lpc.c
  third_party/rnnoise/src/kiss_fft.c third_party/rnnoise/src/pitch.c
  third_party/rnnoise/src/rnn.c third_party/rnnoise/src/rnn_data.c
  third_party/rnnoise/src/rnn_reader.c)
target_include_directories(kk_rnnoise PUBLIC third_party/rnnoise/include
  PRIVATE third_party/rnnoise/src)
target_compile_options(kk_rnnoise PRIVATE -w -O2)
set_target_properties(kk_rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(kinoklip_core PUBLIC kk_rnnoise)
target_include_directories(kinoklip_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
# earcut.hpp (ISC, third_party/earcut/readme.md): triangulates the caps of extruded 3D text.
target_include_directories(kinoklip_core PRIVATE ${CMAKE_SOURCE_DIR}/third_party/earcut)

# qt-libadwaita (GPL-2.0-or-later, third_party/qt-libadwaita/readme.md): the optional
# "Libadwaita (GNOME look)" widget style, built in so it needs no plugin path.
file(GLOB KK_ADWAITA_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/qt-libadwaita/src/*.cpp)
add_library(kk_adwaita STATIC ${KK_ADWAITA_SOURCES}
    ${CMAKE_SOURCE_DIR}/third_party/qt-libadwaita/data/kde-colors/kdecolors.qrc)
set_target_properties(kk_adwaita PROPERTIES AUTOMOC ON AUTORCC ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(kk_adwaita PUBLIC ${CMAKE_SOURCE_DIR}/third_party/qt-libadwaita/src)
target_compile_options(kk_adwaita PRIVATE -w)
target_link_libraries(kk_adwaita PUBLIC Qt6::Widgets Qt6::DBus)
target_link_libraries(kinoklip_core PUBLIC kk_adwaita)
target_compile_definitions(kinoklip_core PUBLIC
  KK_APP_NAME="${APP_NAME}" KK_APP_ID="${APP_ID}" KK_APP_VERSION="${PROJECT_VERSION}"
)
# Where plugins/examples is when running from a build of the source tree (PluginRegistry::examplesDir()).
target_compile_definitions(kinoklip_core PRIVATE KK_PLUGIN_EXAMPLES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/plugins/examples"
  KK_PLUGIN_BUNDLED_SOURCE_DIR="${CMAKE_SOURCE_DIR}/plugins/bundled")
target_link_libraries(kinoklip_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Multimedia Qt6::Svg Qt6::SvgWidgets
  Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Concurrent Qt6::DBus Qt6::Network PkgConfig::FFMPEG)
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
if(KK_ORT_INCLUDE AND KK_ORT_LIBRARY)
  target_include_directories(kinoklip_core PRIVATE ${KK_ORT_INCLUDE})
  target_link_libraries(kinoklip_core PUBLIC ${KK_ORT_LIBRARY})
  target_compile_definitions(kinoklip_core PUBLIC KK_HAVE_ONNXRUNTIME=1)
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

# Translations (resources/translations/kinoklip_<code>.ts): compiled with lrelease into
# :/translations/kinoklip_<code>.qm. `cmake --build . --target update_translations` re-reads
# the sources into the .ts files (docs/contributing.md, "Translating Kinoklip"). Without
# Qt LinguistTools the application is built English-only.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
find_package(Python3 QUIET COMPONENTS Interpreter)
file(GLOB KK_TS_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/resources/translations/*.ts)
if(TARGET Qt6::lrelease AND KK_TS_FILES)
  set(KK_QM_DIR ${CMAKE_BINARY_DIR}/translations)
  file(MAKE_DIRECTORY ${KK_QM_DIR})
  set(KK_QM_FILES "")
  set(KK_QM_QRC "<RCC>\n  <qresource prefix=\"/translations\">\n")
  foreach(ts ${KK_TS_FILES})
    get_filename_component(ts_name ${ts} NAME_WE)
    set(qm ${KK_QM_DIR}/${ts_name}.qm)
    if(Python3_Interpreter_FOUND)
      # scripts/ts_fallback.py: every text also in one context, for the widgets whose tr()
      # context differs between lupdate and run time (see the script).
      add_custom_command(OUTPUT ${qm}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/ts_fallback.py ${ts} ${KK_QM_DIR}/${ts_name}.ts
        COMMAND Qt6::lrelease ${KK_QM_DIR}/${ts_name}.ts -qm ${qm}
        DEPENDS ${ts} ${CMAKE_SOURCE_DIR}/scripts/ts_fallback.py COMMENT "lrelease ${ts_name}" VERBATIM)
    else()
      add_custom_command(OUTPUT ${qm}
        COMMAND Qt6::lrelease ${ts} -qm ${qm}
        DEPENDS ${ts} COMMENT "lrelease ${ts_name}" VERBATIM)
    endif()
    list(APPEND KK_QM_FILES ${qm})
    string(APPEND KK_QM_QRC "    <file>${ts_name}.qm</file>\n")
  endforeach()
  string(APPEND KK_QM_QRC "  </qresource>\n</RCC>\n")
  file(WRITE ${KK_QM_DIR}/translations.qrc "${KK_QM_QRC}")
  add_custom_target(kk_translations DEPENDS ${KK_QM_FILES})
  qt_add_resources(KK_QM_OBJS ${KK_QM_DIR}/translations.qrc)
  target_sources(kinoklip_core PRIVATE ${KK_QM_OBJS})
  add_dependencies(kinoklip_core kk_translations)
  target_compile_definitions(kinoklip_core PUBLIC KK_HAVE_TRANSLATIONS=1)
endif()
if(TARGET Qt6::lupdate AND Python3_Interpreter_FOUND)
  add_custom_target(update_translations
    # scripts/translation_sources.py writes helper_strings.cpp: the texts of the translate()
    # helpers (tr("...") through a function) that lupdate cannot follow on its own.
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/translation_sources.py --write
    COMMAND Qt6::lupdate ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/resources/translations/helper_strings.cpp
            -recursive -locations none -no-obsolete -ts ${KK_TS_FILES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "lupdate: reading the sources into resources/translations/*.ts" VERBATIM)
endif()

# ---- Application ----
add_executable(${APP_ID} src/main.cpp)
target_link_libraries(${APP_ID} PRIVATE kinoklip_core)
set_target_properties(${APP_ID} PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
install(TARGETS ${APP_ID} RUNTIME DESTINATION bin BUNDLE DESTINATION .)
# Example plugins and the plugin manifest schema (docs/plugins); bundled plugins go to
# share/kinoklip/plugins.
install(DIRECTORY plugins/examples/ DESTINATION share/kinoklip/plugin-examples)
install(DIRECTORY plugins/bundled/ DESTINATION share/kinoklip/plugins)
install(FILES docs/plugins/plugin.schema.json DESTINATION share/kinoklip)

# ---- Tests: every tests/**/test_*.cpp becomes its own executable ----
enable_testing()
find_program(KK_DBUS_RUN_SESSION dbus-run-session)
file(GLOB_RECURSE KK_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
foreach(test_src ${KK_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  # Sanitizer builds: scripts/lsan_run.sh gives each test its own sanitizer log file.
  set(KK_TEST_LAUNCH "")
  if(KK_SANITIZE)
    set(KK_TEST_LAUNCH ${CMAKE_SOURCE_DIR}/scripts/lsan_run.sh ${test_name})
  endif()
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE kinoklip_core Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE KK_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
  if(test_name MATCHES "^test_(filedialogs|rendernotify|openurl)$" AND KK_DBUS_RUN_SESSION)
    # their fake portal / notification / OpenURI services need a private session bus (skipped without)
    add_test(NAME ${test_name} COMMAND ${KK_TEST_LAUNCH} ${KK_DBUS_RUN_SESSION} -- $<TARGET_FILE:${test_name}>)
  else()
    add_test(NAME ${test_name} COMMAND ${KK_TEST_LAUNCH} $<TARGET_FILE:${test_name}>)
  endif()
  set(KK_TEST_TIMEOUT 300)
  if(KK_SANITIZE)
    set(KK_TEST_TIMEOUT 3600) # a test property overrides ctest --timeout; sanitized runs are slower
  endif()
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT ${KK_TEST_TIMEOUT})
endforeach()

# Test OpenFX plugins (tests/ofx/kk_test_ofx.cpp): the plain plugin, one that aborts while loading and one that
# hangs while loading, each as a bundle in <build>/ofx-test (what tests/ofx/test_ofx.cpp and test_ofxrender.cpp scan).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(KK_OFX_ARCH "Linux-arm-64")
else()
  set(KK_OFX_ARCH "Linux-x86-64")
endif()
foreach(variant IN ITEMS "" "Crash" "Hang" "Transition")
  set(ofx_target "kk_test_ofx${variant}")
  set(ofx_name "KkTest${variant}")
  add_library(${ofx_target} MODULE tests/ofx/kk_test_ofx.cpp)
  target_include_directories(${ofx_target} PRIVATE ${CMAKE_SOURCE_DIR}/third_party/openfx/include)
  set_target_properties(${ofx_target} PROPERTIES PREFIX "" OUTPUT_NAME "${ofx_name}" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ofx-test/${ofx_name}.ofx.bundle/Contents/${KK_OFX_ARCH}"
    CXX_VISIBILITY_PRESET hidden)
  if(variant STREQUAL "Crash")
    target_compile_definitions(${ofx_target} PRIVATE KK_OFX_CRASH=1)
  elseif(variant STREQUAL "Hang")
    target_compile_definitions(${ofx_target} PRIVATE KK_OFX_HANG=1)
  elseif(variant STREQUAL "Transition")
    target_compile_definitions(${ofx_target} PRIVATE KK_OFX_TRANSITION=1)
  endif()
endforeach()
foreach(t IN ITEMS test_ofx test_ofxrender)
  if(TARGET ${t})
    target_compile_definitions(${t} PRIVATE KK_OFX_TEST_DIR="${CMAKE_BINARY_DIR}/ofx-test"
      KK_OFX_PROBE_PATH="$<TARGET_FILE:kinoklip-ofx-probe>")
    add_dependencies(${t} kk_test_ofx kk_test_ofxCrash kk_test_ofxHang kk_test_ofxTransition kinoklip-ofx-probe)
  endif()
endforeach()

# The hardware encoder probe program (tests/export/test_hwencprobe.cpp): the tests run it for real.
foreach(t IN ITEMS test_hwencprobe test_encoderprobe test_deliverpage)
  if(TARGET ${t})
    target_compile_definitions(${t} PRIVATE KK_HWENC_PROBE_PATH="$<TARGET_FILE:kinoklip-hwenc-probe>")
    add_dependencies(${t} kinoklip-hwenc-probe)
  endif()
endforeach()

# Test audio plugins (tests/audioplug/kk_test_lv2.cpp, kk_test_clap.cpp): a gain, one that aborts while loading and a
# 32 frame delay that reports its latency, each as an LV2 bundle in <build>/audioplug-test/lv2 and a CLAP file in
# <build>/audioplug-test/clap (what tests/audioplug/test_audioplug.cpp scans).
set(KK_AP_DIR "${CMAKE_BINARY_DIR}/audioplug-test")
foreach(variant IN ITEMS gain crash delay)
  string(TOUPPER ${variant} VARIANT)
  add_library(kk_test_clap_${variant} MODULE tests/audioplug/kk_test_clap.cpp)
  target_include_directories(kk_test_clap_${variant} PRIVATE ${CMAKE_SOURCE_DIR}/third_party/clap/include)
  target_compile_definitions(kk_test_clap_${variant} PRIVATE KK_CLAP_${VARIANT}=1)
  if(variant STREQUAL "gain")
    target_compile_definitions(kk_test_clap_${variant} PRIVATE KK_CLAP_PLAIN=1)
  endif()
  set_target_properties(kk_test_clap_${variant} PROPERTIES PREFIX "" OUTPUT_NAME "kk-test-${variant}" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${KK_AP_DIR}/clap" CXX_VISIBILITY_PRESET hidden)
  if(LILV_FOUND)
    add_library(kk_test_lv2_${variant} MODULE tests/audioplug/kk_test_lv2.cpp)
    target_link_libraries(kk_test_lv2_${variant} PRIVATE PkgConfig::LILV)
    target_compile_definitions(kk_test_lv2_${variant} PRIVATE KK_LV2_${VARIANT}=1)
    set_target_properties(kk_test_lv2_${variant} PROPERTIES PREFIX "" OUTPUT_NAME "kk_test_${variant}"
      LIBRARY_OUTPUT_DIRECTORY "${KK_AP_DIR}/lv2/kk-test-${variant}.lv2" CXX_VISIBILITY_PRESET hidden)
    set(uri "urn:kinoklip:test:${variant}")
    file(WRITE "${KK_AP_DIR}/lv2/kk-test-${variant}.lv2/manifest.ttl"
"@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
<${uri}> a lv2:Plugin ; lv2:binary <kk_test_${variant}.so> ; rdfs:seeAlso <kk_test_${variant}.ttl> .
")
    file(WRITE "${KK_AP_DIR}/lv2/kk-test-${variant}.lv2/kk_test_${variant}.ttl"
"@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix doap:  <http://usefulinc.com/ns/doap#> .
@prefix units: <http://lv2plug.in/ns/extensions/units#> .
<${uri}> a lv2:Plugin , lv2:AmplifierPlugin ;
  doap:name \"Kinoklip Test ${variant}\" ;
  lv2:optionalFeature lv2:hardRTCapable ;
  lv2:port [ a lv2:InputPort , lv2:AudioPort ; lv2:index 0 ; lv2:symbol \"in\" ; lv2:name \"In\" ] ,
    [ a lv2:OutputPort , lv2:AudioPort ; lv2:index 1 ; lv2:symbol \"out\" ; lv2:name \"Out\" ] ,
    [ a lv2:InputPort , lv2:ControlPort ; lv2:index 2 ; lv2:symbol \"gain\" ; lv2:name \"Gain\" ;
      lv2:default 0.0 ; lv2:minimum -60.0 ; lv2:maximum 24.0 ; units:unit units:db ] ,
    [ a lv2:InputPort , lv2:ControlPort ; lv2:index 3 ; lv2:symbol \"invert\" ; lv2:name \"Invert\" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled ] ,
    [ a lv2:OutputPort , lv2:ControlPort ; lv2:index 4 ; lv2:symbol \"latency\" ; lv2:name \"Latency\" ;
      lv2:designation lv2:latency ; lv2:portProperty lv2:reportsLatency ] .
")
  endif()
endforeach()
if(TARGET test_audioplug)
  target_compile_definitions(test_audioplug PRIVATE KK_AUDIOPLUG_TEST_DIR="${KK_AP_DIR}"
    KK_AUDIO_PROBE_PATH="$<TARGET_FILE:kinoklip-audio-probe>" KK_AUDIOPLUG_HAVE_LV2=$<BOOL:${LILV_FOUND}>)
  add_dependencies(test_audioplug kinoklip-audio-probe kk_test_clap_gain kk_test_clap_crash kk_test_clap_delay)
  if(LILV_FOUND)
    add_dependencies(test_audioplug kk_test_lv2_gain kk_test_lv2_crash kk_test_lv2_delay)
  endif()
endif()

# The memory soak (also labelled release below) is a long run of its own: `ctest -L soak` (docs/contributing.md, "Memory leaks").
if(TARGET test_memorysoak)
  set_tests_properties(test_memorysoak PROPERTIES TIMEOUT 900)
endif()

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

# test_menureference starts the built application as `kinoklip --menu-reference`.
if(TARGET test_menureference)
  target_compile_definitions(test_menureference PRIVATE KK_BINARY_PATH="$<TARGET_FILE:${APP_ID}>")
  add_dependencies(test_menureference ${APP_ID})
endif()

# The GitHub issue forms (.github/ISSUE_TEMPLATE) and the field ids Help > Report a Bug fills.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME check_issue_forms
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_issue_forms.py)
  # Every text a translate() helper forwards must be in the .ts files, and none unfinished
  # (scripts/translation_sources.py; docs/contributing.md, "Translating Kinoklip").
  add_test(NAME check_translations
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/translation_sources.py --check)
  add_test(NAME check_vim_docs
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/vim-docs.py --check)
  add_test(NAME check_status
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/status.py --check)
  # docs/menu-reference.md is written by the application (kinoklip --menu-reference): a menu
  # entry or panel added without regenerating it fails here.
  add_test(NAME check_menu_reference
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_menu_reference.py
            $<TARGET_FILE:${APP_ID}>)
  set_tests_properties(check_menu_reference PROPERTIES TIMEOUT 300)
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

# Test groups: every test gets exactly one of quick / full / release (tests/groups.cmake,
# docs/building.md "Test groups"). A test in no list gets the label "ungrouped" and
# check_test_groups fails.
include(${CMAKE_SOURCE_DIR}/tests/groups.cmake)
get_property(KK_ALL_TESTS DIRECTORY PROPERTY TESTS)
foreach(t ${KK_ALL_TESTS})
  set(KK_GROUP ungrouped)
  if(t IN_LIST KK_TESTS_RELEASE)
    set(KK_GROUP release)
  elseif(t IN_LIST KK_TESTS_FULL)
    set(KK_GROUP full)
  elseif(t IN_LIST KK_TESTS_QUICK)
    set(KK_GROUP quick)
  endif()
  if(t STREQUAL "test_memorysoak")
    set(KK_GROUP "${KK_GROUP};soak")
  endif()
  set_tests_properties(${t} PROPERTIES LABELS "${KK_GROUP}")
endforeach()
if(Python3_Interpreter_FOUND)
  add_test(NAME check_test_groups
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_test_groups.py ${CMAKE_BINARY_DIR})
  set_tests_properties(check_test_groups PROPERTIES LABELS quick)
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
