; SlipStream Installer Script
; Works with Inno Setup 6.3+

#define MyAppName "SlipStream"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "SlipStream"
#define MyAppExeName "SlipStream.exe"

[Setup]
AppId={{8F3E4A2B-1C5D-4E6F-9A8B-7C2D1E0F3A4B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile=LICENSE-installer.txt
OutputDir=build
OutputBaseFilename=SlipStream-{#MyAppVersion}-Setup
; SetupIconFile=icon.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; === MODERN LOOK ===
WizardStyle=modern
WizardSizePercent=120,100
WizardResizable=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "firewall"; Description: "Add Windows Firewall exception"; GroupDescription: "Network:"; Flags: checkedonce
Name: "startup"; Description: "Start SlipStream when computer boots (before login)"; GroupDescription: "Startup:"; Flags: checkedonce

[Files]
Source: "build\bin\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\bin\Release\*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "build\bin\Release\index.html"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\bin\Release\styles.css"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\bin\Release\js\*"; DestDir: "{app}\js"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Firewall rules
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""SlipStream"" dir=in action=allow program=""{app}\{#MyAppExeName}"" enable=yes"; Flags: runhidden; Tasks: firewall
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""SlipStream"" dir=out action=allow program=""{app}\{#MyAppExeName}"" enable=yes"; Flags: runhidden; Tasks: firewall

; Create scheduled task to run at boot (SYSTEM account, before any user logs in)
Filename: "schtasks"; Parameters: "/create /tn ""SlipStream"" /tr """"""{app}\{#MyAppExeName}"""""" /sc onstart /ru SYSTEM /rl HIGHEST /f"; Flags: runhidden; Tasks: startup

; Launch after install - runs as admin (installer is already elevated)
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent shellexec

[UninstallRun]
; Remove firewall rules
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""SlipStream"""; Flags: runhidden
; Remove scheduled task
Filename: "schtasks"; Parameters: "/delete /tn ""SlipStream"" /f"; Flags: runhidden

[UninstallDelete]
Type: files; Name: "{userappdata}\SlipStream\auth.json"
Type: files; Name: "{userappdata}\SlipStream\jwt_secret.dat"
Type: files; Name: "{userappdata}\SlipStream\server.crt"
Type: files; Name: "{userappdata}\SlipStream\server.key"
Type: dirifempty; Name: "{userappdata}\SlipStream"
Type: dirifempty; Name: "{app}"

[Messages]
WelcomeLabel2=This will install [name/ver] on your computer.%n%nSlipStream provides low-latency remote desktop streaming with hardware-accelerated AV1/HEVC/H.264 encoding.%n%nRequirements:%n• NVIDIA RTX, Intel Arc, or AMD GPU%n• Windows 10/11 (64-bit)