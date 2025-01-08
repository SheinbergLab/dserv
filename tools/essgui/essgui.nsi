; Based on example from 
; 12 06 2005: Luis Wong


; -------------------------------
; Start
 
  !define Name "ESSGui"
  !define MUI_VERSION "1.0"
  !define MUI_BRANDINGTEXT "ESSGui"
  !define MUI_SYSTEM 
  CRCCheck On
 
  ; We should test if we must use an absolute path 
  !include "${NSISDIR}\Contrib\Modern UI\System.nsh"
 
 
;---------------------------------
;General
  
  Name "EssGUI"
  OutFile "install_essgui.exe"
  ShowInstDetails "nevershow"
  ShowUninstDetails "nevershow"
  ;SetCompressor "bzip2"
 
  ;!define MUI_ICON "icon.ico"
  ;!define MUI_UNICON "icon.ico"
  ;!define MUI_SPECIALBITMAP "Bitmap.bmp"
 
 
;--------------------------------
;Folder selection page
 
  InstallDir "$PROGRAMFILES\${Name}"
 
 
;--------------------------------
;Modern UI Configuration
 
  !define MUI_WELCOMEPAGE  
  !define MUI_LICENSEPAGE
  !define MUI_DIRECTORYPAGE
  !define MUI_ABORTWARNING
  !define MUI_UNINSTALLER
  !define MUI_UNCONFIRMPAGE
  !define MUI_FINISHPAGE  
 
 
;--------------------------------
;Language
 
  !insertmacro MUI_LANGUAGE "English"
  
;--------------------------------
;Data
 
  ;LicenseData "Readme.txt"
 
 
;-------------------------------- 
;Installer Sections     
Section "install" 
 
;Add files
  SetOutPath "$INSTDIR"
 
  File "essgui.exe"
  File "tcl90.dll"
  File "dlsh.zip"
  File "zlib1.dll"
  File "libtommath.dll"
  ;File "readme.txt"
 
;create desktop shortcut
  CreateShortCut "$DESKTOP\${Name}.lnk" "$INSTDIR\${Name}.exe" ""
 
;create start-menu items
  CreateDirectory "$SMPROGRAMS\${Name}"
  CreateShortCut "$SMPROGRAMS\${Name}\Uninstall.lnk" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\Uninstall.exe" 0
  CreateShortCut "$SMPROGRAMS\${Name}\${Name}.lnk" "$INSTDIR\${Name}.exe" "" "$INSTDIR\${Name}.exe" 0
 
;write uninstall information to the registry
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${Name}" "DisplayName" "${Name} (remove only)"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${Name}" "UninstallString" "$INSTDIR\Uninstall.exe"
 
  WriteUninstaller "$INSTDIR\Uninstall.exe"
 
SectionEnd
 
 
;--------------------------------    
;Uninstaller Section  
Section "Uninstall"
 
;Delete Files 
  RMDir /r "$INSTDIR\*.*"    
 
;Remove the installation directory
  RMDir "$INSTDIR"
 
;Delete Start Menu Shortcuts
  Delete "$DESKTOP\${Name}.lnk"
  Delete "$SMPROGRAMS\${Name}\*.*"
  RmDir  "$SMPROGRAMS\${Name}"
 
;Delete Uninstaller And Unistall Registry Entries
  DeleteRegKey HKEY_LOCAL_MACHINE "SOFTWARE\${Name}"
  DeleteRegKey HKEY_LOCAL_MACHINE "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\${Name}"  
 
SectionEnd
 
 
;--------------------------------    
;MessageBox Section
 
 
;Function that calls a messagebox when installation finished correctly
Function .onInstSuccess
  MessageBox MB_OK "You have successfully installed ${Name}. Use the desktop icon to start the program."
FunctionEnd
 
Function un.onUninstSuccess
  MessageBox MB_OK "You have successfully uninstalled ${Name}."
FunctionEnd
 
 
;eof