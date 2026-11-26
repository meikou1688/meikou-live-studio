# Meikou Live Studio 2

免費、開源的 Windows 導播軟體。介面是全新設計的導播台，畫面合成、擷取、編碼、推流使用 OBS Studio 的開源核心 libobs。

> 本專案不是 OBS Project 的官方產品，也不隸屬於 OBS Project。

## 下載

到 [Releases](../../releases) 下載最新的 `Setup.exe` 安裝，或下載 `portable.zip` 解壓縮後執行 `bin\64bit\meikou-live-studio.exe`。

- 系統需求：Windows 10／11 64 位元、支援 Direct3D 11 的顯示卡
- 第一次執行 Windows 可能跳出 SmartScreen 警告（安裝檔尚未數位簽章），按「其他資訊 → 仍要執行」
- 和舊版 Meikou Live Studio（Electron 版）分開安裝，可以同時存在

## 目前功能（開發中，M0）

- libobs 即時預覽，固定 1920×1080 輸出
- 訊號源：攝影機／擷取卡／導播機 USB、螢幕擷取、網路影片（RTMP／SRT／RTSP／HLS）
- 音訊：桌面音訊、麥克風（開啟即載入）
- 推流：RTMP／RTMPS（YouTube、Facebook、Twitch、自訂），自動選顯示卡編碼器、斷線自動重連、網路壅塞自動降碼率
- 狀態列：CPU、幀率、掉格、實際推流碼率

串流金鑰只存在你自己的電腦上，不會上傳。

## 回報問題

程式右下角「開啟紀錄資料夾」裡有紀錄檔，回報問題時請附上最新一份。

## 原始碼結構

- `meikou/`：Meikou 程式原始碼（C++／Qt 6）
- `obs-studio/`：OBS Studio 原始碼（子模組，鎖定 33.0.3）
- `installer/`：NSIS 安裝程式腳本
- `.github/workflows/build-windows.yml`：GitHub Actions 自動建置，推到 `main` 就會產出安裝檔並發布到 Releases

建置時會在 `obs-studio/CMakeLists.txt` 加一行 `add_subdirectory(../meikou)`，沿用 OBS 的相依套件與輸出目錄，
並關閉 OBS 自己的介面（`ENABLE_FRONTEND=OFF`）、網頁來源與腳本。

## 授權

本專案以 GNU General Public License v2.0 或更新版本（GPL-2.0-or-later）釋出，詳見 [LICENSE](LICENSE)。
內含的 OBS Studio／libobs、FFmpeg、x264 等元件依其各自的授權條款使用；Qt 6 依 LGPL-3.0 使用。
