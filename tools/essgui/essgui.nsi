;NSIS Modern User Interface
;Basic Example Script
;Written by Joost Verburg

;--------------------------------
;Include Modern UI

  !include "MUI2.nsh"

;--------------------------------
;General

  ;Name and file
  Name "Essgui"
  OutFile "essgui-win.exe"
  Unicode True

  ;Default installation folder
  InstallDir "$LOCALAPPDATA\Essgui"
  
  ;Get installation folder from registry if available
  InstallDirRegKey HKCU "Software\Essgui" ""

  ;Request application privileges for Windows Vista
  RequestExecutionLevel user

;--------------------------------
;Interface Settings

  !define MUI_ABORTWARNING

;--------------------------------
;Pages

  !insertmacro MUI_PAGE_LICENSE "${NSISDIR}\Docs\Modern UI\License.txt"
  !insertmacro MUI_PAGE_COMPONENTS
  !insertmacro MUI_PAGE_DIRECTORY
  !insertmacro MUI_PAGE_INSTFILES
  
  !insertmacro MUI_UNPAGE_CONFIRM
  !insertmacro MUI_UNPAGE_INSTFILES
  
;--------------------------------
;Languages
 
  !insertmacro MUI_LANGUAGE "English"

;--------------------------------
;Installer Sections

Section "Essgui" SecEssgui

  SetOutPath "$INSTDIR"
  
  File "essgui.exe"
  File "tcl90.dll"
  File "dlsh.zip"
  File "zlib1.dll"
  File "libtommath.dll"
  
  ;Store installation folder
  WriteRegStr HKCU "Software\Essgui" "" $INSTDIR
  
  ;Create uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"

SectionEnd

Section "Desktop Shortcut" SectionX
    SetShellVarContext current
    CreateShortCut "$DESKTOP\Essgui.lnk" "$INSTDIR\essgui.exe"
SectionEnd


;--------------------------------
;Uninstaller Section

Section "Uninstall"

  Delete "$INSTDIR\essgui.exe"
  Delete "$INSTDIR\tcl90.dll"
  Delete "$INSTDIR\dlsh.zip"
  Delete "$INSTDIR\zlib1.dll"
  Delete "$INSTDIR\libtommath.dll"
  
  Delete "$INSTDIR\Uninstall.exe"

  RMDir "$INSTDIR"

  DeleteRegKey /ifempty HKCU "Software\Essgui"

SectionEnd
 
