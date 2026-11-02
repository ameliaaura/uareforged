; UA Reforged - Windows installer (NSIS 3, built by packaging/windows/build-windows.sh).
; Per-user install: no administrator prompt, default %LOCALAPPDATA%\Programs\UA Reforged.
; It installs the engine and its launcher only: NO game files. On first start the app asks for the player's own
; Urban Assault folder or fetches the community package from the Metropolis Dawn community (see README-Windows.txt).
; Defines passed in by the build script: STAGE, GEN, OUTFILE, VERSION, ICON, EST_KB.

Unicode true
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!define APPNAME "UA Reforged"
!define STUB "UA Reforged.exe"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\UAReforged"
!define DATADIR "$LOCALAPPDATA\UA Reforged"

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\${APPNAME}"
InstallDirRegKey HKCU "${UNINSTKEY}" "InstallLocation"
BrandingText "${APPNAME} ${VERSION}"
ShowInstDetails nevershow
ShowUninstDetails nevershow
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Engine: GPL-2.0 (OpenUA / UA_source)"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This installs ${APPNAME} ${VERSION} for your Windows account only (no administrator rights needed).$\r$\n$\r$\n${APPNAME} is the 1998 game Urban Assault on the open-source OpenUA engine, with modern graphics. Unofficial, free, non-commercial fan project.$\r$\n$\r$\nIt contains no game files. On first start one step sets everything up: it fetches the free community OpenUA Standalone Package from the Metropolis Dawn community and the Reforge pack for you, or uses your own Urban Assault folder."
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APPNAME} now"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchGame
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README-Windows.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Show the read-me"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
; no folder page and no file list: both would show the player's account folder (it shows when they stream or share
; a screenshot). It always installs to the per-user Programs folder; the zip is there for any other place.
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!include "${GEN}\uninstall-list.nsh"

Function LaunchGame
  SetOutPath "$INSTDIR"
  Exec '"$INSTDIR\${STUB}"'
FunctionEnd

Section "${APPNAME} (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  SetOverwrite on
  File /r "${STAGE}\*"

  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  SetOutPath "$INSTDIR"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${STUB}" "" "$INSTDIR\${STUB}" 0
  CreateShortCut "$SMPROGRAMS\${APPNAME}\Install UA Reforged VR on my Quest.lnk" "$INSTDIR\${STUB}" "-UaQuest" "$INSTDIR\${STUB}" 0
  CreateShortCut "$SMPROGRAMS\${APPNAME}\${APPNAME} - Read me.lnk" "$INSTDIR\README-Windows.txt"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "UA Reforged (OpenUA community engine)"
  WriteRegStr HKCU "${UNINSTKEY}" "URLInfoAbout" "https://uareforged.pages.dev/"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\${STUB}"
  WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" ${EST_KB}
SectionEnd

Section "Desktop shortcut" SecDesktop
  SetOutPath "$INSTDIR"
  CreateShortCut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${STUB}" "" "$INSTDIR\${STUB}" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "The UA Reforged engine and its launcher, with Start menu shortcuts. No game files."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Put a ${APPNAME} shortcut on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; Uninstall: the program files and shortcuts. Your saved games, the downloaded game and the Reforge pack in
; %LOCALAPPDATA%\UA Reforged stay unless you say so (silent: "Uninstall.exe /S /PURGE" removes them too).
; Your own Urban Assault folder is never touched: the app only links to it, and the links are removed as links.
Section "Uninstall"
  StrCpy $0 "keep"
  ${GetParameters} $1
  ClearErrors
  ${GetOptions} $1 "/PURGE" $2
  ${IfNot} ${Errors}
    StrCpy $0 "purge"
  ${ElseIf} ${FileExists} "${DATADIR}\*.*"
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Also delete your UA Reforged saved games and settings, the downloaded game and the Reforge pack (UA Reforged's own data folder)?$\r$\n$\r$\nYour own Urban Assault folder is not touched." /SD IDNO IDNO keepdata
    StrCpy $0 "purge"
    keepdata:
  ${EndIf}
  ${If} $0 == "purge"
  ${AndIf} ${FileExists} "$INSTDIR\launcher\UA-Reforged.ps1"
    ; the launcher removes its folder junction-safe (links are unlinked, never followed)
    nsExec::Exec '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\launcher\UA-Reforged.ps1" -UaPurge'
    Pop $3
  ${EndIf}

  Delete "$DESKTOP\${APPNAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  DeleteRegKey HKCU "${UNINSTKEY}"
  ; only the named items the installer put down (generated list), never a blind delete of the folder
  !insertmacro UA_UNINSTALL_LIST
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
