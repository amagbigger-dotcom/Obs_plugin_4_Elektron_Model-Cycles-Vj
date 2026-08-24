; =========================================================================
; Cycles VJ OBS Studio Plugin Suite Installer Script (22 DLLs)
; =========================================================================

#define MyAppName "Cycles VJ OBS Plugins"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Cycles VJ Project"
#define MyAppExeName "Cycles_VJ_Setup.exe"

[Setup]
; Telepito alapadatok
AppId={{D8A1E73C-59B1-4A63-9B67-C1E91C9A208B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\obs-studio
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes

; Kimeneti beallitasok
OutputDir=Output
OutputBaseFilename=Cycles_VJ_Setup
Compression=lzma2/max
SolidCompression=yes

; Rendszergazdai jogosultsag az Program Files mappaba torteno irashoz
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog

; 64-bites OBS Studio celzasa
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64compatible

; Telepito kinezete es viselkedese
WizardStyle=modern
DisableDirPage=no
DisableReadyPage=no
DisableFinishedPage=no
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\bin\64bit\obs64.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Mind a 22 leforditott DLL masolasa az OBS 64-bites plugin mappajaba
Source: "..\build\Release\*.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion uninsrestartdelete restartreplace

[Icons]
Name: "{group}\Uninstall Cycles VJ Plugins"; Filename: "{uninstallexe}"

[Code]
// Ellenorizzuk, hogy az OBS Studio elerheto-e a kivalasztott konyvtarban
function NextButtonClick(CurPageID: Integer): Boolean;
var
  ObsExePath: String;
begin
  Result := True;
  
  if CurPageID = wpSelectDir then
  begin
    ObsExePath := ExpandConstant('{app}\bin\64bit\obs64.exe');
    if not FileExists(ObsExePath) then
    begin
      if MsgBox('Az OBS Studio nem talalhato ezen az eleresi utvonalon:' #13#10 + 
                ExpandConstant('{app}') + #13#10#13#10 + 
                'Biztosan ide szeretned telepiteni a plugineket?', 
                mbConfirmation, MB_YESNO) = IDNO then
      begin
        Result := False;
      end;
    end;
  end;
end;
