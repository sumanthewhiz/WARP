#include "framework.h"
#include "ContextInference.h"
#include "ActivityDatabase.h"
#include "ForegroundMonitor.h"

#include <ctime>
#include <cwctype>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

// Recompute the snapshot every 60 seconds (cheap -- no ML).  The 15-minute
// rolling window stays in lock-step regardless.
static const DWORD CONTEXT_INTERVAL_MS = 60 * 1000;

// Activity window: last 15 minutes.
static const int64_t CONTEXT_WINDOW_SECS = 15 * 60;

// History cap: 24 hours of 1-min snapshots.  In practice the material-change
// dedup keeps this much smaller.
static const size_t  HISTORY_MAX = 1440;

// History append heartbeat: even if the one-liner hasn't changed, append a
// fresh row at most every 5 minutes so the timeline preserves continuity.
static const int64_t HISTORY_HEARTBEAT_SECS = 5 * 60;

// Maximum #items in the structured breakdown, and length budget for the
// composed one-liner.
static const size_t  MAX_ITEMS         = 5;
static const size_t  ONE_LINER_BUDGET  = 180; // characters

// =====================================================================
//  App / verb classifier  --  layered match (exact exe -> path heuristic
//  -> fallback).  Pure functions, easy to unit-test.
// =====================================================================

namespace {

struct AppClass
{
    const wchar_t* exeLower;     // exact match in lowercased basename
    const char*    friendlyName; // shown in the one-liner
    const char*    verb;         // verb prefix in the one-liner
};

// Layer 1: exact exe-name overrides (most reliable).
static const AppClass kAppClasses[] = {
    // Code editors / IDEs
    { L"devenv.exe",         "Visual Studio",     "Editing"               },
    { L"code.exe",           "VS Code",           "Editing"               },
    { L"notepad++.exe",      "Notepad++",         "Editing"               },
    { L"sublime_text.exe",   "Sublime Text",      "Editing"               },
    { L"atom.exe",           "Atom",              "Editing"               },
    { L"idea64.exe",         "IntelliJ IDEA",     "Editing"               },
    { L"pycharm64.exe",      "PyCharm",           "Editing"               },
    { L"clion64.exe",        "CLion",             "Editing"               },
    { L"webstorm64.exe",     "WebStorm",          "Editing"               },
    { L"goland64.exe",       "GoLand",            "Editing"               },
    { L"rubymine64.exe",     "RubyMine",          "Editing"               },
    { L"phpstorm64.exe",     "PhpStorm",          "Editing"               },
    { L"rider64.exe",        "Rider",             "Editing"               },
    { L"datagrip64.exe",     "DataGrip",          "Editing"               },
    { L"androidstudio64.exe","Android Studio",    "Editing"               },
    { L"eclipse.exe",        "Eclipse",           "Editing"               },
    { L"rstudio.exe",        "RStudio",           "Editing"               },
    { L"matlab.exe",         "MATLAB",            "Working in"            },
    { L"notepad.exe",        "Notepad",           "Editing"               },
    { L"wordpad.exe",        "WordPad",           "Editing"               },
    // Office
    { L"winword.exe",        "Word",              "Drafting"              },
    { L"excel.exe",          "Excel",             "Working on"            },
    { L"powerpnt.exe",       "PowerPoint",        "Editing presentation"  },
    { L"onenote.exe",        "OneNote",           "Taking notes in"       },
    { L"outlook.exe",        "Outlook",           "Reviewing email in"    },
    { L"olk.exe",            "Outlook",           "Reviewing email in"    },
    { L"msaccess.exe",       "Access",            "Working in"            },
    { L"visio.exe",          "Visio",             "Diagramming in"        },
    { L"mspub.exe",          "Publisher",         "Editing in"            },
    { L"lync.exe",           "Skype for Business","Chatting in"           },
    // Communication
    { L"ms-teams.exe",       "Teams",             "On Teams"              },
    { L"teams.exe",          "Teams",             "On Teams"              },
    { L"slack.exe",          "Slack",             "Chatting in"           },
    { L"discord.exe",        "Discord",           "Chatting in"           },
    { L"telegram.exe",       "Telegram",          "Chatting in"           },
    { L"whatsapp.exe",       "WhatsApp",          "Chatting in"           },
    { L"signal.exe",         "Signal",            "Chatting in"           },
    { L"zoom.exe",            "Zoom",             "On Zoom"               },
    { L"webex.exe",          "Webex",             "On Webex"              },
    // Browsers
    { L"chrome.exe",         "Chrome",            "Reading"               },
    { L"msedge.exe",         "Edge",              "Reading"               },
    { L"firefox.exe",        "Firefox",           "Reading"               },
    { L"brave.exe",          "Brave",             "Reading"               },
    { L"opera.exe",          "Opera",             "Reading"               },
    { L"vivaldi.exe",        "Vivaldi",           "Reading"               },
    { L"arc.exe",            "Arc",               "Reading"               },
    // PDFs / docs
    { L"acrord32.exe",       "Acrobat Reader",    "Reading PDF"           },
    { L"acrobat.exe",        "Acrobat",           "Reading PDF"           },
    { L"sumatrapdf.exe",     "Sumatra PDF",       "Reading PDF"           },
    { L"foxitreader.exe",    "Foxit Reader",      "Reading PDF"           },
    // Notes
    { L"notion.exe",         "Notion",            "Taking notes in"       },
    { L"obsidian.exe",       "Obsidian",          "Taking notes in"       },
    { L"evernote.exe",       "Evernote",          "Taking notes in"       },
    { L"logseq.exe",         "Logseq",            "Taking notes in"       },
    // Terminals
    { L"windowsterminal.exe","Windows Terminal",  "Running commands in"   },
    { L"wt.exe",             "Windows Terminal",  "Running commands in"   },
    { L"cmd.exe",            "Command Prompt",    "Running commands in"   },
    { L"powershell.exe",     "PowerShell",        "Running commands in"   },
    { L"pwsh.exe",           "PowerShell",        "Running commands in"   },
    { L"conhost.exe",        "Console",           "Running commands in"   },
    // Design / media
    { L"photoshop.exe",      "Photoshop",         "Editing image in"      },
    { L"illustrator.exe",    "Illustrator",       "Designing in"          },
    { L"figma.exe",          "Figma",             "Designing in"          },
    { L"blender.exe",        "Blender",           "Modeling in"           },
    { L"premiere.exe",       "Premiere Pro",      "Editing video in"      },
    { L"afterfx.exe",        "After Effects",     "Compositing in"        },
    { L"spotify.exe",        "Spotify",           "Listening in"          },
    { L"vlc.exe",             "VLC",              "Watching in"           },
    { L"wmplayer.exe",       "Media Player",      "Listening in"          },
    // Dev tooling
    { L"git-bash.exe",       "Git Bash",          "Running commands in"   },
    { L"docker desktop.exe", "Docker Desktop",    "Working in"            },
    { L"docker.exe",         "Docker",            "Running commands in"   },
    { L"warp.exe",           "Warp Terminal",     "Running commands in"   },
    { L"github desktop.exe", "GitHub Desktop",    "Reviewing changes in"  },
    { L"sourcetree.exe",     "Sourcetree",        "Reviewing changes in"  },
    // VMs / remote
    { L"mstsc.exe",          "Remote Desktop",    "On Remote Desktop"     },
    { L"vmware.exe",          "VMware",           "Working in"            },
    { L"virtualbox.exe",     "VirtualBox",        "Working in"            },
};

// Helpers ------------------------------------------------------------

static std::wstring ToLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static std::string  Utf8Lower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return out;
}

static std::string ExeBasenameUtf8(const std::wstring& exeNameW)
{
    // ContextInference receives exeName already as "foo.exe" (no path).
    // Just convert to UTF-8.
    if (exeNameW.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, exeNameW.c_str(), -1,
                                  nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len - 1 : 0, '\0');
    if (len > 0)
        WideCharToMultiByte(CP_UTF8, 0, exeNameW.c_str(), -1,
                            &s[0], len, nullptr, nullptr);
    return s;
}

// True if this exe should be ignored when computing the user's context.
// These are the same shell/system processes ForegroundMonitor already
// filters from emission; we list them again here as defense-in-depth
// against any that did leak through.
static bool IsBoringExe(const std::wstring& exeLower)
{
    static const wchar_t* const kBoring[] = {
        L"explorer.exe", L"dwm.exe", L"shellexperiencehost.exe",
        L"startmenuexperiencehost.exe", L"applicationframehost.exe",
        L"textinputhost.exe", L"lockapp.exe", L"logonui.exe",
        L"taskhostw.exe", L"runtimebroker.exe", L"sihost.exe",
        L"ctfmon.exe", L"widgets.exe", L"widgetservice.exe",
        L"searchhost.exe", L"searchapp.exe", L"searchui.exe",
        L"consent.exe", L"smartscreen.exe", L"securityhealthsystray.exe",
        L"werfault.exe", L"wermgr.exe", L"warp!.exe",
    };
    for (const auto* b : kBoring) if (exeLower == b) return true;
    return false;
}

// Layer-1 lookup: exact basename match.  Returns nullptr if no override.
static const AppClass* MatchExactExe(const std::wstring& exeLower)
{
    for (const auto& a : kAppClasses)
    {
        if (exeLower == a.exeLower) return &a;
    }
    return nullptr;
}

// Layer-2 lookup: exe path heuristics.  Catches JetBrains toolbox variants,
// VS Insiders, Office click-to-run, etc., that don't match the exact
// kAppClasses table.  `pathLower` is the lowercased full exePath.
static const AppClass* MatchByPath(const std::wstring& pathLower)
{
    static const AppClass kJetbrains  = { nullptr, "JetBrains IDE",     "Editing" };
    static const AppClass kVisualStud = { nullptr, "Visual Studio",     "Editing" };
    static const AppClass kVsCode     = { nullptr, "VS Code",           "Editing" };
    static const AppClass kOffice     = { nullptr, "Office",            "Working in" };
    static const AppClass kEdgeChan   = { nullptr, "Edge",              "Reading" };
    static const AppClass kChromeChan = { nullptr, "Chrome",            "Reading" };

    if (pathLower.find(L"\\jetbrains\\") != std::wstring::npos
     || pathLower.find(L"\\toolbox\\apps\\") != std::wstring::npos)
        return &kJetbrains;

    if (pathLower.find(L"\\microsoft visual studio\\") != std::wstring::npos)
        return &kVisualStud;

    if (pathLower.find(L"\\code - insiders\\") != std::wstring::npos
     || pathLower.find(L"\\microsoft vs code") != std::wstring::npos)
        return &kVsCode;

    if (pathLower.find(L"\\microsoft office\\") != std::wstring::npos
     || pathLower.find(L"\\microsoft\\office\\") != std::wstring::npos)
        return &kOffice;

    if (pathLower.find(L"\\microsoft\\edge") != std::wstring::npos)
        return &kEdgeChan;

    if (pathLower.find(L"\\google\\chrome") != std::wstring::npos)
        return &kChromeChan;

    return nullptr;
}

// Returns (friendlyName, verb).  Layer order: exact exe -> path -> fallback.
static std::pair<std::string, std::string>
ClassifyApp(const std::wstring& exeName, const std::wstring& exePath)
{
    std::wstring exeLower  = ToLower(exeName);
    std::wstring pathLower = ToLower(exePath);

    if (const AppClass* a = MatchExactExe(exeLower))
        return { a->friendlyName, a->verb };

    if (const AppClass* a = MatchByPath(pathLower))
        return { a->friendlyName, a->verb };

    // Fallback: friendly name = exe basename without ".exe".
    std::string base = ExeBasenameUtf8(exeName);
    if (base.size() > 4
     && _stricmp(base.c_str() + base.size() - 4, ".exe") == 0)
        base.erase(base.size() - 4);
    if (base.empty()) base = "an app";
    return { base, "Working in" };
}

// True if this app is a web browser (drives the title-override path that
// uses BrowsingActivity.title instead of the raw window title).
static bool IsBrowser(const std::wstring& exeLower,
                      const std::wstring& pathLower)
{
    static const wchar_t* const kBrowsers[] = {
        L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe",
        L"opera.exe", L"vivaldi.exe", L"arc.exe",
    };
    for (const auto* b : kBrowsers) if (exeLower == b) return true;
    if (pathLower.find(L"\\microsoft\\edge") != std::wstring::npos) return true;
    if (pathLower.find(L"\\google\\chrome")  != std::wstring::npos) return true;
    return false;
}

// =====================================================================
//  Title cleaner
//
//  Window titles have wildly different formats per app:
//    * "FileMonitor.cpp - WARP! - Microsoft Visual Studio"
//    * "Inbox - Suman Ghosh - Outlook"
//    * "GitHub Copilot CLI -- documentation - Microsoft Edge"
//    * "WARP - Visual Studio Code"
//    * "Meeting | Microsoft Teams"
//    * "● ContextInference.cpp"
//
//  Strategy:
//    1. Normalize separators (em-dash, en-dash, pipe, bullet) -> " - ".
//    2. Split on " - ".
//    3. Drop trailing segments that look like an app suffix (matches the
//       app friendly name OR a known noisy-suffix pattern).
//    4. If a single segment remains, return it (trimmed + length-capped).
//       Otherwise join the remaining segments with " - ".
// =====================================================================
static std::string NormalizeSeparators(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    // Walk byte-by-byte.  UTF-8 multi-byte sequences for em/en dash and
    // bullet are matched as raw byte triples.
    for (size_t i = 0; i < in.size(); )
    {
        // em-dash (U+2014)  e2 80 94
        // en-dash (U+2013)  e2 80 93
        // bullet  (U+2022)  e2 80 a2
        if (i + 2 < in.size()
         && (unsigned char)in[i]   == 0xE2
         && (unsigned char)in[i+1] == 0x80
         && ((unsigned char)in[i+2] == 0x94
          || (unsigned char)in[i+2] == 0x93
          || (unsigned char)in[i+2] == 0xA2))
        {
            out += " - ";
            i += 3;
            continue;
        }
        if (in[i] == '|')
        {
            out += " - ";
            ++i;
            continue;
        }
        out += in[i];
        ++i;
    }
    return out;
}

static std::string Trim(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    size_t b = s.size();
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
    return s.substr(a, b - a);
}

static std::vector<std::string> SplitOnDash(const std::string& s)
{
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= s.size())
    {
        size_t p = s.find(" - ", i);
        if (p == std::string::npos)
        {
            parts.push_back(Trim(s.substr(i)));
            break;
        }
        parts.push_back(Trim(s.substr(i, p - i)));
        i = p + 3;
    }
    return parts;
}

// Suffixes that are *always* removable (don't carry user-meaningful info).
static const std::vector<std::string> kStripSuffixes = {
    "microsoft visual studio", "visual studio", "visual studio code",
    "vs code", "microsoft edge", "google chrome", "mozilla firefox",
    "firefox", "edge", "chrome", "opera", "brave", "vivaldi", "arc",
    "microsoft outlook", "outlook", "microsoft word", "word",
    "microsoft excel", "excel", "microsoft powerpoint", "powerpoint",
    "microsoft onenote", "onenote", "microsoft teams", "teams",
    "microsoft access", "microsoft visio", "microsoft publisher",
    "slack", "discord", "telegram", "whatsapp", "signal",
    "notepad", "notepad++", "sublime text", "atom",
    "intellij idea", "pycharm", "clion", "webstorm", "goland",
    "rubymine", "phpstorm", "rider", "datagrip", "android studio",
    "eclipse", "rstudio", "matlab", "spotify", "vlc",
    "windows terminal", "command prompt", "powershell", "console",
    "github desktop", "sourcetree", "remote desktop",
    "adobe acrobat reader", "adobe acrobat", "acrobat reader",
    "acrobat", "sumatra pdf", "foxit reader",
    "notion", "obsidian", "evernote", "logseq",
    "photoshop", "illustrator", "figma", "blender",
    "premiere pro", "after effects",
};

static bool LooksLikeAppSuffix(const std::string& seg, const std::string& friendlyName)
{
    std::string lower = Utf8Lower(seg);
    if (lower.empty()) return true;
    if (Utf8Lower(friendlyName) == lower) return true;
    for (const auto& s : kStripSuffixes)
    {
        if (lower == s) return true;
    }
    return false;
}

static std::string CleanTitle(const std::string& title,
                              const std::string& friendlyName)
{
    if (title.empty()) return "";

    std::string normalized = NormalizeSeparators(title);
    auto parts = SplitOnDash(normalized);
    if (parts.empty()) return Trim(normalized);

    // Strip leading status glyphs commonly prepended by editors when a
    // file has unsaved changes (e.g. VS Code's "● ", "* ").
    if (!parts[0].empty())
    {
        // UTF-8 bullet = e2 97 8f
        if (parts[0].size() >= 3
         && (unsigned char)parts[0][0] == 0xE2
         && (unsigned char)parts[0][1] == 0x97
         && (unsigned char)parts[0][2] == 0x8F)
            parts[0] = Trim(parts[0].substr(3));
        else if (parts[0].size() >= 2 && parts[0][0] == '*' && parts[0][1] == ' ')
            parts[0] = Trim(parts[0].substr(2));
    }

    // Drop trailing app-suffix segments.
    while (parts.size() > 1 && LooksLikeAppSuffix(parts.back(), friendlyName))
        parts.pop_back();

    // Drop leading segments that are pure app-suffix noise (some apps put
    // the app name first, e.g. "Microsoft Teams - Meeting").
    while (parts.size() > 1 && LooksLikeAppSuffix(parts.front(), friendlyName))
        parts.erase(parts.begin());

    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i) joined += " - ";
        joined += parts[i];
    }
    joined = Trim(joined);

    // Length cap.
    static const size_t kTitleMax = 80;
    if (joined.size() > kTitleMax)
        joined = joined.substr(0, kTitleMax - 1) + "\xE2\x80\xA6"; // "..."

    return joined;
}

// =====================================================================
//  Per-app aggregation.  Combines focus events (from DB) + the live
//  active session (from ForegroundMonitor) + a browsing-title override
//  for browser apps.
// =====================================================================
struct AppAgg
{
    std::wstring exeName;           // canonical, kept for path fallback
    std::wstring exePath;
    std::string  friendlyName;
    std::string  verb;
    int          totalFocusSecs = 0;
    int64_t      lastSeenTs     = 0;
    std::string  bestTitle;         // cleaned, ready for one-liner
    bool         isBrowser      = false;
};

} // namespace

// =====================================================================
ContextInference::ContextInference()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

ContextInference::~ContextInference()
{
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

bool ContextInference::Init()
{
    // No model files, no remote dependencies.  Always succeeds.
    return true;
}

void ContextInference::Start(ActivityDatabase* db, ForegroundMonitor* fg)
{
    if (m_running) return;
    m_db = db;
    m_fg = fg;
    m_running = true;
    ResetEvent(m_stopEvent);
    m_thread = std::thread(&ContextInference::TimerLoop, this);
}

void ContextInference::Stop()
{
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);
    if (m_thread.joinable()) m_thread.join();
}

void ContextInference::TimerLoop()
{
    RunOnce();
    while (m_running)
    {
        DWORD r = WaitForSingleObject(m_stopEvent, CONTEXT_INTERVAL_MS);
        if (r == WAIT_OBJECT_0 || !m_running) break;
        RunOnce();
    }
}

void ContextInference::RunOnce()
{
    if (!m_db) return;

    ContextSnapshot snap = ComposeSnapshot();

    std::lock_guard<std::mutex> lk(m_mutex);
    m_latest     = snap;
    m_haveLatest = true;

    if (ShouldAppendToHistory(snap))
    {
        m_history.push_back(snap);
        if (m_history.size() > HISTORY_MAX)
            m_history.erase(m_history.begin(),
                            m_history.begin() + (m_history.size() - HISTORY_MAX));
    }
}

bool ContextInference::ShouldAppendToHistory(const ContextSnapshot& snap) const
{
    // Suppress empty / no-activity rows from history entirely; the
    // "latest" cache will still report them.
    if (snap.activityCount == 0) return false;

    if (m_history.empty()) return true;

    const ContextSnapshot& prev = m_history.back();

    // Append on heartbeat regardless (preserves continuity).
    if (snap.timestamp - prev.timestamp >= HISTORY_HEARTBEAT_SECS)
        return true;

    // Append on material change: different one-liner OR different dominant app.
    if (snap.oneLiner != prev.oneLiner) return true;
    if (!snap.items.empty() && !prev.items.empty()
     && snap.items.front().exe != prev.items.front().exe)
        return true;

    return false;
}

// =====================================================================
//  Snapshot composition
// =====================================================================
ContextSnapshot ContextInference::ComposeSnapshot()
{
    ContextSnapshot snap;
    snap.timestamp     = static_cast<int64_t>(std::time(nullptr));
    snap.windowEndTs   = snap.timestamp;
    snap.windowStartTs = snap.timestamp - CONTEXT_WINDOW_SECS;

    // ---- Gather raw signals ---------------------------------------
    auto focus  = m_db->QueryAppFocusCustomSeconds(CONTEXT_WINDOW_SECS);
    auto browse = m_db->QueryBrowsingCustomSeconds(CONTEXT_WINDOW_SECS);
    auto apps   = m_db->QueryAppLaunchesCustomSeconds(CONTEXT_WINDOW_SECS);
    auto files  = m_db->QueryFilesCustomSeconds(CONTEXT_WINDOW_SECS);

    std::unordered_set<std::string> sigs;
    if (!focus.empty())  sigs.insert("app_focus");
    if (!browse.empty()) sigs.insert("browsing");
    if (!apps.empty())   sigs.insert("app_launch");
    if (!files.empty())  sigs.insert("file");
    snap.signalTypes.assign(sigs.begin(), sigs.end());
    std::sort(snap.signalTypes.begin(), snap.signalTypes.end());

    snap.activityCount = static_cast<int>(focus.size() + browse.size()
                                        + apps.size()  + files.size());

    // ---- Live-session overlay (critical for accuracy) -------------
    // ForegroundMonitor only emits a row on focus *change*.  If the user
    // has been parked in the same window for the whole 15-min slice we
    // would see an empty focus list -- so we ask the monitor for the
    // currently-tracked session and synthesize a virtual focus row from
    // it that overlaps with the rolling window.
    if (m_fg)
    {
        ForegroundMonitor::ActiveSession active;
        if (m_fg->GetCurrentSession(active))
        {
            int64_t startTs = active.startedAtUtcSecs > 0
                ? active.startedAtUtcSecs
                : (snap.timestamp - active.durationSoFarSecs);
            int64_t effectiveStart = (std::max)(startTs, snap.windowStartTs);
            int     dwellInWindow  = static_cast<int>(snap.timestamp - effectiveStart);
            if (dwellInWindow > 0)
            {
                AppFocusActivity virt{};
                virt.id           = -1;
                virt.timestampUtc = effectiveStart;
                virt.exeName      = active.exeName;
                virt.exePath      = active.exePath;
                virt.windowTitle  = active.windowTitle;
                virt.durationSecs = dwellInWindow;
                focus.push_back(virt);
                if (snap.signalTypes.empty() || snap.signalTypes[0] != "app_focus")
                {
                    if (std::find(snap.signalTypes.begin(),
                                  snap.signalTypes.end(),
                                  "app_focus") == snap.signalTypes.end())
                    {
                        snap.signalTypes.push_back("app_focus");
                        std::sort(snap.signalTypes.begin(), snap.signalTypes.end());
                    }
                }
                snap.activityCount += 1;
            }
        }
    }

    // ---- Aggregate by exe -----------------------------------------
    std::unordered_map<std::string, AppAgg> byExe; // key: lowercased exe basename

    int totalFocusSecs = 0;

    for (const auto& f : focus)
    {
        std::wstring exeLower  = ToLower(f.exeName);
        std::wstring pathLower = ToLower(f.exePath);
        if (IsBoringExe(exeLower)) continue;

        std::string key = ExeBasenameUtf8(exeLower);
        auto& agg = byExe[key];
        if (agg.exeName.empty())
        {
            agg.exeName = f.exeName;
            agg.exePath = f.exePath;
            auto cls = ClassifyApp(f.exeName, f.exePath);
            agg.friendlyName = cls.first;
            agg.verb         = cls.second;
            agg.isBrowser    = IsBrowser(exeLower, pathLower);
        }
        agg.totalFocusSecs += (std::max)(0, f.durationSecs);
        totalFocusSecs     += (std::max)(0, f.durationSecs);
        if (f.timestampUtc > agg.lastSeenTs)
        {
            agg.lastSeenTs = f.timestampUtc;
            std::string title = WideToUtf8(f.windowTitle);
            agg.bestTitle = CleanTitle(title, agg.friendlyName);
        }
    }

    // ---- Browser title override -----------------------------------
    // Browser window titles often look like "GitHub - Microsoft Edge" --
    // BrowsingMonitor extracts a cleaner title that's more representative
    // of the active tab.  For each browser app, find the most recent
    // browsing record from any browser instance and prefer that title.
    if (!browse.empty())
    {
        std::sort(browse.begin(), browse.end(),
                  [](const BrowsingActivity& a, const BrowsingActivity& b){
                      return a.timestampUtc > b.timestampUtc;
                  });
        for (auto& kv : byExe)
        {
            if (!kv.second.isBrowser) continue;
            // Pick the most recent browsing event (any browser); good
            // enough for the one-liner.  Could be tightened to match
            // browser by name, but Chrome/Edge/Firefox all share the
            // "title bar" layout so cross-matching is acceptable.
            for (const auto& b : browse)
            {
                std::string t = WideToUtf8(b.title);
                if (t.empty()) continue;
                kv.second.bestTitle = CleanTitle(t, kv.second.friendlyName);
                break;
            }
        }
    }

    snap.focusSeconds = totalFocusSecs;

    // ---- Score & rank apps ----------------------------------------
    std::vector<AppAgg> ranked;
    ranked.reserve(byExe.size());
    for (auto& kv : byExe) ranked.push_back(std::move(kv.second));
    std::sort(ranked.begin(), ranked.end(),
              [](const AppAgg& a, const AppAgg& b){
                  if (a.totalFocusSecs != b.totalFocusSecs)
                      return a.totalFocusSecs > b.totalFocusSecs;
                  return a.lastSeenTs > b.lastSeenTs;
              });

    if (!ranked.empty() && totalFocusSecs > 0)
        snap.dominantPct = static_cast<int>(
            (ranked.front().totalFocusSecs * 100LL) / totalFocusSecs);

    // ---- Structured breakdown -------------------------------------
    for (size_t i = 0; i < ranked.size() && snap.items.size() < MAX_ITEMS; ++i)
    {
        ContextSnapshot::AppItem it;
        it.app          = ranked[i].friendlyName;
        it.exe          = ExeBasenameUtf8(ToLower(ranked[i].exeName));
        it.title        = ranked[i].bestTitle;
        it.focusSeconds = ranked[i].totalFocusSecs;
        it.pct          = totalFocusSecs > 0
            ? static_cast<int>((ranked[i].totalFocusSecs * 100LL) / totalFocusSecs)
            : 0;
        snap.items.push_back(std::move(it));
    }

    // ---- One-liner composition ------------------------------------
    auto phraseFor = [](const AppAgg& a) -> std::string {
        if (a.bestTitle.empty())
            return std::string(a.verb) + " " + a.friendlyName;
        return std::string(a.verb) + " \"" + a.bestTitle + "\" in " + a.friendlyName;
    };

    if (ranked.empty())
    {
        // No usable focus signal -- fall back to a recent file edit, or
        // an app launch, or report idle.
        if (!files.empty())
        {
            // Pick the most recent file.
            auto it = std::max_element(files.begin(), files.end(),
                [](const FileActivity& a, const FileActivity& b){
                    return a.timestampUtc < b.timestampUtc;
                });
            std::string p = WideToUtf8(it->path);
            size_t slash = p.find_last_of("/\\");
            std::string base = (slash != std::string::npos) ? p.substr(slash + 1) : p;
            snap.oneLiner = "Editing \"" + base + "\"";
            snap.confidence = 0.35;
        }
        else if (!apps.empty())
        {
            auto it = std::max_element(apps.begin(), apps.end(),
                [](const AppLaunchActivity& a, const AppLaunchActivity& b){
                    return a.timestampUtc < b.timestampUtc;
                });
            auto cls = ClassifyApp(it->exeName, it->exePath);
            snap.oneLiner = "Launched " + cls.first;
            snap.confidence = 0.25;
        }
        else
        {
            snap.oneLiner   = "(no recent user activity)";
            snap.confidence = 0.0;
        }
        return snap;
    }

    // Adaptive top-N: include apps until we cover ~80 % of focus time
    // OR the character budget is hit.  Always include at least one.
    std::string oneLiner;
    int covered = 0;
    size_t included = 0;
    for (size_t i = 0; i < ranked.size(); ++i)
    {
        std::string phrase = phraseFor(ranked[i]);
        std::string candidate = oneLiner.empty()
            ? phrase
            : oneLiner + " \xC2\xB7 " + phrase; // " · "
        if (i > 0
         && (candidate.size() > ONE_LINER_BUDGET
          || (totalFocusSecs > 0
              && static_cast<int>((covered * 100LL) / totalFocusSecs) >= 80)))
            break;
        oneLiner = candidate;
        covered += ranked[i].totalFocusSecs;
        ++included;
    }

    if (included < ranked.size())
    {
        size_t remaining = ranked.size() - included;
        std::ostringstream o;
        o << oneLiner << " \xC2\xB7 + " << remaining
          << " other app" << (remaining == 1 ? "" : "s");
        oneLiner = o.str();
    }

    snap.oneLiner = oneLiner;

    // ---- Confidence -----------------------------------------------
    // Heuristic: 0 if no focus, ~0.3 with a small amount of focus, up
    // to ~0.95 when the dominant app has high focus % AND we have
    // multiple signal types.  Capped at 0.99 so the value never reads
    // as "absolutely certain".
    double byTime    = (std::min)(1.0, totalFocusSecs / 600.0); // 10 min -> 1.0
    double byDom     = snap.dominantPct / 100.0;
    double bySignals = (std::min)(1.0, snap.signalTypes.size() / 3.0);
    double conf      = 0.5 * byTime + 0.3 * byDom + 0.2 * bySignals;
    if (conf > 0.99) conf = 0.99;
    snap.confidence  = conf;

    return snap;
}

// =====================================================================
//  Query API
// =====================================================================
std::string ContextInference::SnapshotToJsonObject(const ContextSnapshot& s)
{
    std::ostringstream o;
    o << "{"
      << "\"timestamp\":"       << s.timestamp
      << ",\"window_start\":"   << s.windowStartTs
      << ",\"window_end\":"     << s.windowEndTs
      << ",\"one_liner\":\""    << EscapeJson(s.oneLiner) << "\""
      << ",\"activity_count\":" << s.activityCount
      << ",\"focus_seconds\":"  << s.focusSeconds
      << ",\"dominant_focus_pct\":" << s.dominantPct
      << ",\"confidence\":"     << s.confidence
      << ",\"signal_types\":[";
    for (size_t i = 0; i < s.signalTypes.size(); ++i)
    {
        if (i) o << ",";
        o << "\"" << EscapeJson(s.signalTypes[i]) << "\"";
    }
    o << "],\"items\":[";
    for (size_t i = 0; i < s.items.size(); ++i)
    {
        if (i) o << ",";
        const auto& it = s.items[i];
        o << "{\"app\":\""  << EscapeJson(it.app)
          << "\",\"exe\":\"" << EscapeJson(it.exe)
          << "\",\"title\":\"" << EscapeJson(it.title)
          << "\",\"focus_seconds\":" << it.focusSeconds
          << ",\"pct\":" << it.pct << "}";
    }
    o << "]}";
    return o.str();
}

std::string ContextInference::GetRecentContext()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::ostringstream o;
    o << "{\"recent_context\":";
    if (m_haveLatest)
        o << SnapshotToJsonObject(m_latest);
    else
        o << "null";
    o << ",\"history_count\":" << m_history.size() << "}";
    return o.str();
}

std::string ContextInference::GetRecentContexts(int count)
{
    if (count <= 0)  count = 10;
    if (count > 200) count = 200;

    std::lock_guard<std::mutex> lk(m_mutex);

    std::vector<const ContextSnapshot*> latestFirst;
    for (auto it = m_history.rbegin();
         it != m_history.rend() && (int)latestFirst.size() < count;
         ++it)
        latestFirst.push_back(&*it);

    std::ostringstream o;
    o << "{\"recent_contexts\":[";
    for (size_t i = 0; i < latestFirst.size(); ++i)
    {
        if (i) o << ",";
        o << SnapshotToJsonObject(*latestFirst[i]);
    }
    o << "],\"returned\":" << latestFirst.size()
      << ",\"history_count\":" << m_history.size()
      << ",\"requested\":"     << count << "}";
    return o.str();
}

void ContextInference::ClearHistory()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_history.clear();
    m_latest = ContextSnapshot{};
    m_haveLatest = false;
}

// =====================================================================
//  Helpers
// =====================================================================
std::string ContextInference::WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len - 1 : 0, '\0');
    if (len > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

std::string ContextInference::EscapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                out += buf;
            }
            else
            {
                out += c;
            }
            break;
        }
    }
    return out;
}
