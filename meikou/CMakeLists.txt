cmake_minimum_required(VERSION 3.28...3.30)

# Meikou Live Studio：以 libobs 為核心的導播軟體（Windows／macOS）。
# 這個目錄由 CI 掛進 OBS 的建置樹（obs-studio/CMakeLists.txt 加一行 add_subdirectory），
# 沿用 OBS 的相依套件、輸出目錄、DLL／Framework 與核心模組打包流程。
# 目標名稱刻意叫 obs-studio：OBS 的 set_target_properties_obs 只對這個名稱做
# 「複製所有相依函式庫與核心模組、產生 .app」的處理；實際輸出名稱在下面改掉。

if(NOT MEIKOU_VERSION)
  set(MEIKOU_VERSION "2.0.0-dev")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" MEIKOU_VERSION_NUMERIC "${MEIKOU_VERSION}")
string(REGEX MATCH "[0-9]+$" MEIKOU_BUILD_NUMBER "${MEIKOU_VERSION}")

find_package(Qt6 REQUIRED Widgets)

add_executable(obs-studio WIN32)

target_sources(
  obs-studio
  PRIVATE
    src/main.cpp
    src/Log.cpp
    src/Log.hpp
    src/Platform.hpp
    src/Engine.cpp
    src/Engine.hpp
    src/Display.cpp
    src/Display.hpp
    src/AudioMixer.cpp
    src/AudioMixer.hpp
    src/PropertiesDialog.cpp
    src/PropertiesDialog.hpp
    src/MainWindow.cpp
    src/MainWindow.hpp
    src/GraphicsRender.cpp
    src/GraphicsRender.hpp
    src/GraphicSource.cpp
    src/GraphicSource.hpp
    src/OverlayDialog.cpp
    src/OverlayDialog.hpp
    src/SelfTest.cpp
    src/SelfTest.hpp
    src/FlowLayout.cpp
    src/FlowLayout.hpp
    src/MediaControl.hpp
    src/NetMedia.cpp
    src/NetMedia.hpp
    src/Updater.cpp
    src/Updater.hpp
    resources/meikou.qrc
)

# 內建字型：思源黑體 Noto Sans TC（Black／Bold，SIL Open Font License）
set(
  MEIKOU_FONTS
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/NotoSansTC-Black.ttf"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/NotoSansTC-Bold.ttf"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/OFL.txt"
)

if(OS_WINDOWS)
  target_sources(obs-studio PRIVATE resources/meikou.rc resources/meikou.manifest)
  target_compile_definitions(obs-studio PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_link_libraries(obs-studio PRIVATE OBS::libobs Qt::Widgets)

target_compile_definitions(obs-studio PRIVATE MEIKOU_VERSION="${MEIKOU_VERSION}")

set_target_properties(
  obs-studio
  PROPERTIES AUTOMOC TRUE AUTOUIC FALSE AUTORCC TRUE COMPILE_WARNING_AS_ERROR OFF
)

set_target_properties_obs(obs-studio PROPERTIES FOLDER meikou OUTPUT_NAME meikou-live-studio)

if(OS_MACOS)
  # 和 OBS 官方介面一樣，用實際的 dylib 檔名載入繪圖引擎
  if(TARGET OBS::libobs-opengl)
    target_compile_definitions(obs-studio PRIVATE MEIKOU_DL_OPENGL="$<TARGET_SONAME_FILE_NAME:OBS::libobs-opengl>")
  endif()
  target_sources(obs-studio PRIVATE ${MEIKOU_FONTS})
  set_source_files_properties(${MEIKOU_FONTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/fonts)
else()
  # 字型放在主程式旁邊的 fonts 資料夾（rundir/<設定>/bin/64bit/fonts）
  add_custom_command(
    TARGET obs-studio
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${MEIKOU_FONTS} "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}/fonts/"
    COMMENT "Copy Meikou fonts"
    VERBATIM
  )
endif()

if(OS_MACOS)
  string(TIMESTAMP MEIKOU_YEAR "%Y")
  set_target_properties(obs-studio PROPERTIES OUTPUT_NAME "Meikou Live Studio")
  set_target_xcode_properties(
    obs-studio
    PROPERTIES PRODUCT_NAME "Meikou Live Studio"
               PRODUCT_BUNDLE_IDENTIFIER com.meikou.live-studio
               MARKETING_VERSION ${MEIKOU_VERSION_NUMERIC}
               CURRENT_PROJECT_VERSION ${MEIKOU_BUILD_NUMBER}
               GCC_WARN_INHIBIT_ALL_WARNINGS YES
               INFOPLIST_KEY_CFBundleDisplayName "Meikou Live Studio"
               INFOPLIST_KEY_NSHumanReadableCopyright "(c) ${MEIKOU_YEAR} Meikou. GPL-2.0-or-later. Built on libobs."
               INFOPLIST_KEY_NSCameraUsageDescription "Meikou Live Studio 需要使用攝影機／擷取卡，才能把畫面放進導播。"
               INFOPLIST_KEY_NSMicrophoneUsageDescription "Meikou Live Studio 需要使用麥克風／音訊介面，才能收音與直播。"
               INFOPLIST_KEY_NSAppleEventsUsageDescription "Meikou Live Studio 需要這個權限，才能在背景使用快捷鍵。"
  )
endif()
