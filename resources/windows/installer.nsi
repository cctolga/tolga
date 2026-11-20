; Tolga Player installer (NSIS), made by scripts/make-windows.sh:
;   makensis -DVERSION=x.y.z -DSTAGE=<program folder> -DOUTFILE=<installer.exe> -DICON=<tolga.ico> installer.nsi
; Installs for the current user only (no administrator rights), into
; %LOCALAPPDATA%\Programs\Tolga Player, with a Start menu entry and "Open with" for videos.
Unicode true
!include "MUI2.nsh"

!define APP      "Tolga Player"
!define EXE      "Tolga Player.exe"
!define PROGID   "TolgaPlayer.Media"
!define UNINST   "Software\Microsoft\Windows\CurrentVersion\Uninstall\TolgaPlayer"
; Windows' "Default apps" settings list Tolga Player with these types (the app can open that page)
!define CAPS     "Software\Tolga Player\Capabilities"

Name "${APP}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP}"
InstallDirRegKey HKCU "${UNINST}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "${APP} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Apache License 2.0"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; the file types Tolga offers to open ("Open with" list; the default app is not changed)
!macro FileTypes _do
  !insertmacro ${_do} ".mkv"
  !insertmacro ${_do} ".mp4"
  !insertmacro ${_do} ".m4v"
  !insertmacro ${_do} ".mov"
  !insertmacro ${_do} ".avi"
  !insertmacro ${_do} ".webm"
  !insertmacro ${_do} ".ts"
  !insertmacro ${_do} ".m2ts"
  !insertmacro ${_do} ".mts"
  !insertmacro ${_do} ".mpg"
  !insertmacro ${_do} ".mpeg"
  !insertmacro ${_do} ".vob"
  !insertmacro ${_do} ".wmv"
  !insertmacro ${_do} ".flv"
  !insertmacro ${_do} ".ogv"
  !insertmacro ${_do} ".3gp"
!macroend
!macro AddType _ext
  WriteRegStr HKCU "Software\Classes\${_ext}\OpenWithProgids" "${PROGID}" ""
  WriteRegStr HKCU "Software\Classes\Applications\${EXE}\SupportedTypes" "${_ext}" ""
  WriteRegStr HKCU "${CAPS}\FileAssociations" "${_ext}" "${PROGID}"
!macroend
!macro RemoveType _ext
  DeleteRegValue HKCU "Software\Classes\${_ext}\OpenWithProgids" "${PROGID}"
!macroend

Section "Install"
  ; a running copy would keep its files locked
  nsExec::Exec 'taskkill /im "${EXE}" /f'
  SetOutPath "$INSTDIR"
  RMDir /r "$INSTDIR\Licenses"
  File /r "${STAGE}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\${EXE}"

  WriteRegStr HKCU "Software\Classes\${PROGID}" "" "Video"
  WriteRegStr HKCU "Software\Classes\${PROGID}" "FriendlyTypeName" "Video"
  WriteRegStr HKCU "Software\Classes\${PROGID}\DefaultIcon" "" '"$INSTDIR\${EXE}",0'
  WriteRegStr HKCU "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\${EXE}" "%1"'
  WriteRegStr HKCU "Software\Classes\Applications\${EXE}" "FriendlyAppName" "${APP}"
  WriteRegStr HKCU "Software\Classes\Applications\${EXE}\shell\open\command" "" '"$INSTDIR\${EXE}" "%1"'
  WriteRegStr HKCU "${CAPS}" "ApplicationName" "${APP}"
  WriteRegStr HKCU "${CAPS}" "ApplicationDescription" "Plays videos and sends them to an Apple TV"
  !insertmacro FileTypes AddType
  WriteRegStr HKCU "Software\RegisteredApplications" "${APP}" "${CAPS}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'  ; SHCNE_ASSOCCHANGED

  WriteRegStr HKCU "${UNINST}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST}" "Publisher" "Tolga C."
  WriteRegStr HKCU "${UNINST}" "URLInfoAbout" "https://cctolga.github.io/tolga/"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" '"$INSTDIR\${EXE}",0'
  WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /im "${EXE}" /f'
  Delete "$SMPROGRAMS\${APP}.lnk"
  !insertmacro FileTypes RemoveType
  DeleteRegKey HKCU "Software\Classes\${PROGID}"
  DeleteRegKey HKCU "Software\Classes\Applications\${EXE}"
  DeleteRegValue HKCU "Software\RegisteredApplications" "${APP}"
  DeleteRegKey HKCU "${CAPS}"   ; only the Capabilities key: the trial record next to it stays
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  DeleteRegKey HKCU "${UNINST}"
  ; settings, license and recent files in %APPDATA%\Tolga stay, as with other apps
  RMDir /r "$INSTDIR"
SectionEnd
