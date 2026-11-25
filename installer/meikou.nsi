; Meikou Live Studio 2（libobs 原生版）安裝程式
; 由 CI 呼叫：makensis /DVERSION=2.0.0-m0.1 /DVIVERSION=2.0.0.1 /DSRCDIR=<rundir> /DOUTFILE=<exe> installer\meikou.nsi
Unicode true
ManifestDPIAware true
!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "2.0.0-dev"
!endif
!ifndef VIVERSION
  !define VIVERSION "2.0.0.0"
!endif

!define APPNAME "Meikou Live Studio 2"
!define APPEXE "meikou-live-studio.exe"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\MeikouLiveStudio2"

Name "${APPNAME}"
Caption "${APPNAME} ${VERSION} 安裝程式"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\${APPNAME}"
InstallDirRegKey HKCU "Software\${APPNAME}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 64
BrandingText "${APPNAME} ${VERSION}"

VIProductVersion "${VIVERSION}"
VIAddVersionKey /LANG=1028 "ProductName" "${APPNAME}"
VIAddVersionKey /LANG=1028 "FileDescription" "${APPNAME} 安裝程式"
VIAddVersionKey /LANG=1028 "CompanyName" "Meikou"
VIAddVersionKey /LANG=1028 "LegalCopyright" "Meikou. Built on libobs (GPL-2.0-or-later)."
VIAddVersionKey /LANG=1028 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1028 "ProductVersion" "${VERSION}"

!define MUI_ICON "..\meikou\resources\meikou.ico"
!define MUI_UNICON "..\meikou\resources\meikou.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "安裝 ${APPNAME} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "這是以 OBS 核心（libobs）打造的 Meikou 原生版，和舊的 Electron 版分開安裝，可以同時存在。$\r$\n$\r$\n不需要系統管理員權限。$\r$\n$\r$\n按「下一步」繼續。"
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\64bit\${APPEXE}"
!define MUI_FINISHPAGE_RUN_TEXT "立即開啟 ${APPNAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "TradChinese"

Section "Install"
  nsExec::Exec 'taskkill /IM "${APPEXE}" /F'
  Sleep 500
  ; 舊版的外掛與資料先清掉，避免殘留不相容的檔案
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\obs-plugins"
  RMDir /r "$INSTDIR\data"
  SetOutPath "$INSTDIR"
  File /r /x "*.pdb" /x "*.lib" /x "*.exp" "${SRCDIR}\*"
  File "/oname=meikou.ico" "..\meikou\resources\meikou.ico"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; 捷徑的工作目錄要在 bin\64bit
  SetOutPath "$INSTDIR\bin\64bit"
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\bin\64bit\${APPEXE}" "" "$INSTDIR\meikou.ico"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\解除安裝 ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\bin\64bit\${APPEXE}" "" "$INSTDIR\meikou.ico"

  WriteRegStr HKCU "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "Meikou"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\meikou.ico"
  WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" "$0"
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /IM "${APPEXE}" /F'
  Sleep 500
  Delete "$DESKTOP\${APPNAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "Software\${APPNAME}"
SectionEnd
