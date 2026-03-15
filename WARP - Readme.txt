WARP - Windows Activity Reasoning Platform
===========================================

WARP is a lightweight Windows desktop application that silently monitors
file/folder activity, application launches, and browsing activity on the
local PC.  It stores everything in a rolling 30-day on-disk database and
exposes a query-able named-pipe API so that other applications on the same
machine can programmatically retrieve activity history.


REQUIREMENTS
------------
  - Windows 10 or later
  - Administrator privileges (a UAC prompt is shown on launch)


FEATURES
--------
  * System-tray residence
      Launches hidden.  A light-bulb tray icon provides "Open" and "Exit"
      right-click menu items.  Double-click the icon to open the window.

  * File-system monitoring
      Every fixed, removable, and network drive is watched recursively.
      Detected actions: CREATE, OPEN, DELETE, MODIFY, RENAME.

  * App launch monitoring
      New user-initiated process launches are detected every 2 seconds.
      System and background processes are automatically filtered out.

  * Browsing activity monitoring
      When a recognised browser (Chrome, Edge, Firefox, Brave, Opera,
      Vivaldi, Internet Explorer) is in the foreground, the page title
      and URL are captured and deduplicated.

  * Idle / sleep awareness
      Monitoring pauses automatically when the PC has been idle for 2+
      minutes or enters sleep/hibernate, and resumes on activity/wake.

  * 30-day rolling storage
      All events are stored in a local SQLite database.  Records older
      than 30 days are automatically purged on startup and every 6 hours.

  * Inference engine
      Every captured event incrementally updates per-entity analytics
      (files, apps, URLs) with access counts and an exponential-decay
      recency score (0-255).

  * Light / Dark mode
      Click the theme toggle button in the top-right corner to switch
      between light and dark themes instantly.

  * Named-pipe query API
      Other Windows processes can connect to
          \\.\pipe\WarpFileActivityAPI
      and retrieve activity data or inference records as JSON.


USAGE
-----
  1. Run WARP!.exe  (accept the UAC elevation prompt).
  2. WARP appears as a light-bulb icon in the system tray and begins
     monitoring immediately.
  3. Right-click the tray icon:
       "Open"  - opens the main window (maximized).
       "Exit"  - shuts down WARP cleanly.
  4. In the main window you can:
       - Click any predefined time-window button (Last 15 min, Last 30 min,
         Last 1 hour, ... Last 30 days) to query recent activity.
       - Enter a custom number of seconds and click "Send Custom Query".
       - Click "Send Default Query" to retrieve the last 1 hour.
       - Use the Event Type checkboxes (File Activity, App Launches,
         Browsing Activity) to filter which categories are returned.
       - Use the "Explore Precomputed Inferences" section to browse the
         top entities ranked by recency score, or look up a specific file
         path / app path / URL.
  5. Query results appear as formatted JSON in the response panel.


DATA LOCATION
-------------
  Database:  %LOCALAPPDATA%\WARP\activity.db


NAMED-PIPE API (QUICK REFERENCE)
---------------------------------
  Pipe name:  \\.\pipe\WarpFileActivityAPI
  Mode:       Message mode (PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE)

  Send a UTF-8 JSON request; receive a UTF-8 JSON response.

  Query by predefined window:
      {"window":"15m"}        Last 15 minutes
      {"window":"30m"}        Last 30 minutes
      {"window":"1h"}         Last 1 hour
      {"window":"2h"}         Last 2 hours
      {"window":"6h"}         Last 6 hours
      {"window":"24h"}        Last 24 hours
      {"window":"7d"}         Last 7 days
      {"window":"15d"}        Last 15 days
      {"window":"30d"}        Last 30 days

  Query by custom seconds:
      {"seconds":300}         Last 5 minutes

  Default (last 1 hour):
      {}

  Filter by event type (optional "types" array):
      {"window":"1h","types":["file","app_launch","browsing"]}

  Inference lookup:
      {"op":"QueryInferences","paths":["c:\\path\\to\\file"]}

  Inference deltas (incremental sync):
      {"op":"GetInferenceDeltas","since_version":0}


AUTHOR
------
  Suman Ghosh