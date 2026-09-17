; Lape's Eye — Instalator Windows (NSIS)
Unicode True

!ifndef APP_VERSION
  !define APP_VERSION "0.5.8"
!endif
!ifndef DEPLOY_DIR
  !define DEPLOY_DIR "..\deploy"
!endif

!define APP_NAME        "Lape's Eye"
!define APP_EXE         "lapes-eye.exe"
!define APP_PUBLISHER   "Lape Photography Tools"
!define APP_URL         "https://github.com/ArtoriasAI/Windows"
!define APP_GUID        "{7A3B2C1D-4E5F-6789-ABCD-EF0123456789}"
!define REG_KEY         "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_GUID}"

Name                "${APP_NAME} ${APP_VERSION}"
OutFile             "lapes-eye-setup-${APP_VERSION}.exe"
InstallDir          "$PROGRAMFILES64\LapesEye"
InstallDirRegKey    HKLM "${REG_KEY}" "InstallLocation"
RequestExecutionLevel admin
SetCompressor       /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"

!define MUI_ABORTWARNING
!define MUI_ICON    "..\resources\icons\lapes-eye.ico"
!define MUI_UNICON  "..\resources\icons\lapes-eye.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "Polish"
!insertmacro MUI_LANGUAGE "English"

Section "Lape's Eye" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${DEPLOY_DIR}\bin\*.*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
  CreateDirectory "$SMPROGRAMS\${APP_PUBLISHER}"
  CreateShortcut "$SMPROGRAMS\${APP_PUBLISHER}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
  CreateShortcut "$SMPROGRAMS\${APP_PUBLISHER}\Odinstaluj ${APP_NAME}.lnk" "$INSTDIR\uninstall.exe"
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegStr   HKLM "${REG_KEY}" "DisplayName"      "${APP_NAME} ${APP_VERSION}"
  WriteRegStr   HKLM "${REG_KEY}" "DisplayVersion"   "${APP_VERSION}"
  WriteRegStr   HKLM "${REG_KEY}" "Publisher"        "${APP_PUBLISHER}"
  WriteRegStr   HKLM "${REG_KEY}" "URLInfoAbout"     "${APP_URL}"
  WriteRegStr   HKLM "${REG_KEY}" "InstallLocation"  "$INSTDIR"
  WriteRegStr   HKLM "${REG_KEY}" "UninstallString"  "$INSTDIR\uninstall.exe"
  WriteRegStr   HKLM "${REG_KEY}" "DisplayIcon"      "$INSTDIR\${APP_EXE}"
  WriteRegDWORD HKLM "${REG_KEY}" "EstimatedSize"    "$0"
  WriteRegDWORD HKLM "${REG_KEY}" "NoModify"         1
  WriteRegDWORD HKLM "${REG_KEY}" "NoRepair"         1
SectionEnd

Section "Uninstall"
  RMDir /r "$INSTDIR"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_PUBLISHER}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_PUBLISHER}\Odinstaluj ${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_PUBLISHER}"
  DeleteRegKey HKLM "${REG_KEY}"
SectionEnd
