cmake_minimum_required(VERSION 3.28...3.30)

# Meikou Live Studio：以 libobs 為核心的導播軟體（自用）。
# 這個目錄由 CI 掛進 OBS 的建置樹（obs-studio/CMakeLists.txt 加一行 add_subdirectory），
# 沿用 OBS 的相依套件、輸出目錄與 Qt／FFmpeg DLL 複製流程。
# 目標名稱刻意叫 obs-studio：OBS 的 set_target_properties_obs 只對這個名稱做
# 「複製所有相依 DLL 與核心模組」的處理；實際輸出檔名是 meikou-live-studio.exe。

if(NOT MEIKOU_VERSION)
  set(MEIKOU_VERSION "2.0.0-dev")
endif()

find_package(Qt6 REQUIRED Widgets)

add_executable(obs-studio WIN32)

target_sources(
  obs-studio
  PRIVATE
    src/main.cpp
    src/Log.cpp
    src/Log.hpp
    src/ObsCore.cpp
    src/ObsCore.hpp
    src/PreviewWidget.cpp
    src/PreviewWidget.hpp
    src/MainWindow.cpp
    src/MainWindow.hpp
    resources/meikou.rc
    resources/meikou.manifest
)

target_link_libraries(obs-studio PRIVATE OBS::libobs Qt::Widgets)

target_compile_definitions(obs-studio PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN MEIKOU_VERSION="${MEIKOU_VERSION}")

set_target_properties(obs-studio PROPERTIES AUTOMOC TRUE AUTOUIC FALSE AUTORCC FALSE)

set_target_properties_obs(obs-studio PROPERTIES FOLDER meikou OUTPUT_NAME meikou-live-studio)
