# Meikou Live Studio 2

Meikou 自用的導播軟體：介面是自己寫的 Qt 程式，畫面合成、擷取、編碼、推流使用 OBS 的核心 libobs。

- `meikou/`：Meikou 程式原始碼（C++／Qt 6）
- `obs-studio/`：OBS Studio 原始碼（子模組，鎖定 33.0.3）
- `installer/`：NSIS 安裝程式腳本
- `.github/workflows/build-windows.yml`：在 GitHub Actions 的 Windows 執行器上編譯，產出安裝檔與免安裝 zip

## 建置方式

推到 `main` 分支就會自動建置。建置時會在 `obs-studio/CMakeLists.txt` 加一行 `add_subdirectory(../meikou)`，
沿用 OBS 的相依套件與輸出目錄，關閉 OBS 自己的介面（`ENABLE_FRONTEND=OFF`）、網頁來源與腳本。

## 授權

libobs 為 GPL-2.0-or-later。本專案僅供自用，不對外散布。
