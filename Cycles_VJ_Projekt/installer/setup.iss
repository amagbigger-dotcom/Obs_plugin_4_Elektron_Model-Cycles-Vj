[Setup]
AppName=Cycles VJ OBS Plugin
AppVersion=1.0.0
DefaultDirName={autopf}\obs-studio
DefaultGroupName=Cycles VJ
OutputBaseFilename=Cycles_VJ_Setup
OutputDir=Output
ArchitecturesInstallIn64BitMode=x64
DisableDirPage=no

[Files]
Source: "..\build\Release\cycles_vj.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
