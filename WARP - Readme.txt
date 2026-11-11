WARP - Windows Activity Reasoning Platform (version 6.0.0)
==========================================================

WARP is a background agent for Windows 10/11. It notices which files,
apps and web pages you work with on this PC, keeps that history in a
local database, works out what you are working on right now, and lets
other programs on this PC (for example search) ask for it through a
local API. Everything stays on this PC: no cloud calls, no telemetry.

What it records: files you open, create, save, download, rename or
delete; app starts and exits; which window and document is in front
and for how long; browser page titles and privacy-filtered web
addresses; whether you are active, away or locked; names of printed
documents; optionally (off by default) clipboard metadata - which app
copied what kind of data. It never records file contents, keystrokes,
screenshots or clipboard content.

PHASE A SIGNAL CORRECTNESS (DEV)
------------------------------
  Storage schema 3 separates active, passive, idle and unknown focus time.
  Zero active time stays zero; overlapping focus/presence is counted once.
  URL paths and opaque document IDs preserve case. Ambiguous file names
  do not identify a specific file. Sparse opens/pages/launches remain useful
  context observations without invented duration or edits.
  Print jobs require a qualified own-user owner and a local client; missing
  or bare owner names are omitted rather than attributed by guesswork.
  Existing data migrates automatically. API clients must honor feed resets
  and must not lowercase arbitrary web keys. See SCHEMA.md and API.md.
  Developers can run tests\run_signal_quality.ps1 from a source checkout.


PRIVACY
-------
  * On first start WARP asks for consent. If you decline, it starts
    paused and records nothing until you choose "Resume recording".
  * Privacy gates suppress Private / InPrivate / Incognito windows.
    Detection depends on browser/Windows UI indicators; see the runtime
    coverage limitations in the project's README.
  * Sign-in, password-manager, banking and patient-portal web sites are
    never recorded. Web addresses are stored without query strings,
    fragments or tokens.
  * Password managers and Windows sign-in / consent screens are never
    recorded. You can exclude more apps, window titles, web sites and
    folders (see SETTINGS).
  * Pause or delete history at any time (tray icon or command line);
    "Delete activity > Everything..." securely wipes the database.
  * Raw events are kept 30 days; per-item history (rankings, work
    threads) 395 days. Both are configurable.
  * The data folder is readable only by you, SYSTEM and Administrators,
    and is EFS-encrypted where Windows supports it.
  * Only programs on this PC can use the API; IT can restrict which ones.


INSTALL
-------
  Requires Windows 10 or 11 and administrator approval once.
  1. Unzip the package for your PC's architecture (x64, ARM64 or x86)
     to any folder.
  2. Open a command prompt in that folder and run:

         WARP!.exe --install

     Approve the administrator prompt. WARP copies itself to
     %ProgramFiles%\WARP, registers a scheduled task
     ("\WARP\WARP Agent - <your user SID>") that starts it at every
     sign-in with administrator rights (needed for file and app-launch
     capture), and starts it right away. You can then delete the
     unzipped folder.
  3. Every Windows user who wants WARP runs --install once.

  Double-clicking WARP!.exe also works: it offers to install, or starts
  WARP if it is already installed.


UNINSTALL
---------
  "%ProgramFiles%\WARP\WARP!.exe" --uninstall
      Stops WARP, removes the scheduled task and the program files.
  "%ProgramFiles%\WARP\WARP!.exe" --uninstall --remove-data
      Also deletes your history (%LOCALAPPDATA%\WARP).


TRAY ICON
---------
  Right-click the light-bulb icon in the notification area:
    Open                   show the WARP window (or double-click the icon)
    Status                 show health and settings
    Pause for 1 hour
    Pause until tomorrow   (until 06:00)
    Resume recording       (shown while paused)
    Delete activity        Last 15 minutes / Last hour / Today / Everything...
    Exit                   stop WARP until your next sign-in

  Tooltip: recording, paused or degraded. Closing the window only hides it.


COMMAND LINE
------------
  WARP!.exe --status                print status (JSON)
  WARP!.exe --start                 start the installed agent now
  WARP!.exe --pause <minutes>       pause recording (default 60)
  WARP!.exe --resume                resume recording
  WARP!.exe --purge-hours <n>       delete the last n hours
  WARP!.exe --purge-domain <host>   delete a web site and its subdomains
  WARP!.exe --purge-path <folder>   delete everything under a folder
  WARP!.exe --install  or  --uninstall [--remove-data]

  These talk to the WARP running in your session; output goes to the
  console, or to a file when redirected.


SETTINGS
--------
  Registry:  HKCU\Software\WARP            your settings
             HKLM\Software\Policies\WARP   IT policy (overrides and locks)
  Common values (DWORD unless noted):
    RawRetentionDays          30   raw history, 1-3650 days
    EntityRetentionDays       395  per-item history and threads, 7-3650 days
    AwayAfterSeconds          300  no input this long = away
    CaptureFiles, CaptureAppLaunch, CaptureFocus, CaptureWeb,
    CapturePrint              1    set 0 to turn a capture source off
    CaptureClipboardMetadata  0    set 1 to record clipboard metadata
    EncryptDataDir            1    EFS-encrypt the data folder
    LlmEnabled                0    optional on-device language model (x64/ARM64)
    LogLevel                  1    0 debug ... 3 error
    ExcludedApps, ExcludedTitlePatterns, ExcludedDomains,
    ExcludedPathPrefixes      REG_MULTI_SZ lists of things never to record
    ApiAllowedClients, ApiActivityClients (REG_MULTI_SZ) and
    ApiRestrictActivityScope  limit which programs may use the API
  WARP re-reads these values every 30 seconds. Exclusions, pause,
  retention and file capture apply right away; turning app-launch, web
  or focus capture on or off applies at the next start (tray > Exit,
  then WARP!.exe --start). The project's README.md lists every value.


DATA AND LOGS
-------------
  History:  %LOCALAPPDATA%\WARP\activity.db
  Logs:     %LOCALAPPDATA%\WARP\logs\warp.log   (rotating)
  Crashes:  %LOCALAPPDATA%\WARP\dumps\          (WARP restarts itself)
  Program:  %ProgramFiles%\WARP\


API (FOR DEVELOPERS)
--------------------
  Local named pipes, message mode, UTF-8 JSON (API version 2):
    \\.\pipe\WarpFileActivityAPI-<SessionId>   preferred (your Windows session id)
    \\.\pipe\WarpFileActivityAPI               legacy, same session only
  Examples:
    {"op":"Ping"}
    {"op":"GetCurrentContext"}
    {"op":"TopEntities","type":"file","limit":20}
    {"window":"1h"}                   version-1 raw event query
  See API.md for every operation.


THIRD-PARTY NOTICES
-------------------
  See NOTICE.md and the "licenses" folder (SQLite, ONNX Runtime,
  ONNX Runtime GenAI, granite-embedding-small-english-r2, Qwen3-0.6B).
  SHA256SUMS.txt lists the SHA-256 hash of every file in this package.


AUTHOR
------
  Suman Ghosh