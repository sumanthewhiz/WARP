#include "framework.h"
#include "ContextInference.h"
#include "Attention.h"
#include "LlmSummarizer.h"
#include "ActivityStore.h"
#include "EntityStore.h"
#include "EntityKey.h"
#include "HistoryEngine.h"
#include "FocusTracker.h"
#include "Settings.h"
#include "Json.h"
#include "Logger.h"

#include <onnxruntime_cxx_api.h>

#include <ctime>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <array>
#include <functional>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

// Default compose cadence (seconds); Settings::ContextCadenceSeconds overrides (15..3600).
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

// Sentence-encoder embedding pipeline constants.
static const int     EMBED_DIM         = 384;
static const int     MAX_SEQ_LEN       = 128;

// Semantic-clustering threshold: per-app phrases whose embeddings have
// cosine similarity >= this value are merged into one "thread of work".
// 0.65 is generous enough to merge `auth.cpp` editing with `Auth PR review`
// in a browser, but tight enough not to merge unrelated apps.
static const float   CLUSTER_COS_THRESHOLD = 0.65f;

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
    // Image viewers / light image editors -- feed the "Files" facet
    // when the title carries a real image filename.
    { L"photos.exe",                 "Photos",       "Viewing"            },
    { L"microsoft.photos.exe",       "Photos",       "Viewing"            },
    { L"irfanview.exe",              "IrfanView",    "Viewing"            },
    { L"i_view64.exe",               "IrfanView",    "Viewing"            },
    { L"i_view32.exe",               "IrfanView",    "Viewing"            },
    { L"mspaint.exe",                "Paint",        "Editing image in"   },
    { L"paint.exe",                  "Paint",        "Editing image in"   },
    { L"paintdotnet.exe",            "Paint.NET",    "Editing image in"   },
    { L"gimp.exe",                   "GIMP",         "Editing image in"   },
    { L"gimp-2.10.exe",              "GIMP",         "Editing image in"   },
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
// These are the same shell/system processes FocusTracker already
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

// True if this app's friendly name represents a *file app* — an
// editor / viewer / authoring tool whose foreground titles are
// dominated by document, file, image, or canvas names.  This is one of
// the signals for routing an activity to the "Files" facet of the
// per-category one-liner; the other signal is title-based file-extension
// detection (`TitleLooksLikeFileActivity` below) which catches
// arbitrary apps whose window title includes a recognized filename
// (e.g. `report.docx - WordPad`, `chart.xlsx - LibreOffice Calc`).
//
// Browsers are intentionally excluded (they feed the "Websites" facet).
// Apps known to be communications, media, terminals, remote desktop, or
// other non-file engagement go to the "Apps" facet via
// `IsAppOnlyFriendlyName` below, even if their title happens to mention
// a filename.
static bool IsFileAppFriendlyName(const std::string& friendlyName)
{
    static const std::unordered_set<std::string> kFileApps = {
        // Code editors / IDEs
        "visual studio", "vs code", "notepad++", "sublime text",
        "atom", "intellij idea", "pycharm", "clion", "webstorm",
        "goland", "rubymine", "phpstorm", "rider", "datagrip",
        "android studio", "eclipse", "rstudio", "matlab",
        "notepad", "wordpad",
        // Office authoring (Outlook is communications -> Apps)
        "word", "excel", "powerpoint", "onenote", "access",
        "visio", "publisher",
        // PDF / document viewers
        "acrobat reader", "acrobat", "sumatra pdf", "foxit reader",
        // Notes / knowledge bases
        "notion", "obsidian", "evernote", "logseq",
        // Design / image / video / 3D authoring
        "photoshop", "illustrator", "figma", "blender",
        "premiere pro", "after effects",
        // Image viewers / light editors
        "photos", "irfanview", "paint", "paint.net", "gimp",
        "windows photo viewer",
        // Virtual entries injected from file activity (FileCapture)
        "document", "file",
    };
    std::string lower = Utf8Lower(friendlyName);
    return kFileApps.find(lower) != kFileApps.end();
}

// True if this app's friendly name represents a non-file engagement
// app: communications (email / chat / video calls), media players,
// terminals, remote desktop, version-control UIs.  These always go to
// the "Apps" facet, even if their window title happens to contain a
// filename token (e.g. a Teams chat message mentioning `report.pdf`).
// This is the second leg of the disjoint Files / Apps partition.
static bool IsAppOnlyFriendlyName(const std::string& friendlyName)
{
    static const std::unordered_set<std::string> kCommsAndSystem = {
        // Email / chat / meetings
        "outlook", "teams", "slack", "discord", "telegram",
        "whatsapp", "signal", "skype for business",
        "zoom", "webex",
        // Media / streaming
        "spotify", "vlc", "media player",
        // Terminals (they show command lines, not document edits)
        "windows terminal", "command prompt", "powershell", "console",
        "git bash", "warp terminal",
        // VMs / remote
        "remote desktop", "vmware", "virtualbox",
        // Containers / dev infra UIs
        "docker desktop", "docker",
        // Version control UIs (they show repos, not files-being-edited)
        "github desktop", "sourcetree",
    };
    std::string lower = Utf8Lower(friendlyName);
    return kCommsAndSystem.find(lower) != kCommsAndSystem.end();
}

// Heuristic: does this window title look like the user has a real file
// open?  Detects "name.ext" patterns where `ext` is a known file
// extension (documents, spreadsheets, slides, PDFs, images, code, data,
// config) with a proper word boundary on both sides.  This lets us
// route an activity to the "Files" facet even when the host app isn't
// in the explicit file-app whitelist -- e.g. someone opens a `.psd1`
// in a generic editor we don't recognize.
static bool TitleLooksLikeFileActivity(const std::string& title)
{
    if (title.size() < 4) return false;
    static const char* const kExts[] = {
        // Documents
        ".doc", ".docx", ".rtf", ".odt",
        ".xls", ".xlsx", ".csv", ".tsv", ".ods",
        ".ppt", ".pptx", ".odp",
        ".pdf", ".epub", ".mobi",
        ".txt", ".md", ".rst", ".tex", ".one",
        // Images
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tif", ".tiff",
        ".webp", ".heic", ".heif", ".raw", ".dng", ".cr2", ".nef",
        ".psd", ".ai", ".eps", ".svg", ".ico",
        // Audio / video (lighter; mostly captured via media players in Apps)
        ".mp3", ".wav", ".flac", ".m4a", ".aac",
        ".mp4", ".mkv", ".mov", ".avi", ".webm",
        // Code / data / config
        ".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".hxx",
        ".cs", ".java", ".kt", ".scala", ".groovy",
        ".py", ".rb", ".pl", ".php", ".js", ".mjs", ".cjs",
        ".jsx", ".ts", ".tsx",
        ".go", ".rs", ".swift", ".m", ".mm", ".dart",
        ".sh", ".bash", ".zsh", ".fish",
        ".ps1", ".psm1", ".psd1",
        ".html", ".htm", ".css", ".scss", ".sass", ".less",
        ".xml", ".json", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf",
        ".sql", ".graphql", ".vue", ".svelte",
        ".r", ".jl", ".lua", ".ex", ".exs", ".erl", ".hs", ".clj",
        // Archives (someone is browsing/extracting a file)
        ".zip", ".rar", ".7z", ".tar", ".gz", ".tgz",
    };
    // Lowercase ASCII inline (Utf8Lower is fine but this is hot enough).
    std::string lo;
    lo.reserve(title.size());
    for (unsigned char c : title)
        lo.push_back((c >= 'A' && c <= 'Z') ? char(c + 32) : char(c));

    for (const char* ext : kExts)
    {
        size_t extLen = std::strlen(ext);
        size_t pos = 0;
        while ((pos = lo.find(ext, pos)) != std::string::npos)
        {
            size_t end = pos + extLen;
            // Word boundary on RIGHT: next char must not be alphanumeric
            // (so ".js" doesn't match inside ".jsonp" and ".c" doesn't
            // match inside ".cpp").
            bool rightBoundary = (end == lo.size()) ||
                !((lo[end] >= 'a' && lo[end] <= 'z') ||
                  (lo[end] >= '0' && lo[end] <= '9'));
            // Word boundary on LEFT: char before the '.' must be a
            // letter/digit/underscore/hyphen (a real filename character),
            // and there must be at least 2 such chars before the dot.
            bool leftBoundary = false;
            if (pos >= 2)
            {
                char prev = lo[pos - 1];
                if ((prev >= 'a' && prev <= 'z') ||
                    (prev >= '0' && prev <= '9') ||
                     prev == '_' || prev == '-')
                {
                    char prev2 = lo[pos - 2];
                    if ((prev2 >= 'a' && prev2 <= 'z') ||
                        (prev2 >= '0' && prev2 <= '9') ||
                         prev2 == '_' || prev2 == '-' || prev2 == ' ')
                        leftBoundary = true;
                }
            }
            if (rightBoundary && leftBoundary) return true;
            pos = end;
        }
    }
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
//  active session (from FocusTracker). Unresolved browser windows keep
//  their own observed title rather than borrowing a different page.
// =====================================================================
struct AppAgg
{
    std::wstring exeName;           // canonical, kept for path fallback
    std::wstring exePath;
    std::wstring aumid;
    std::string  friendlyName;
    std::string  verb;
    int          totalFocusSecs = 0;
    int          exposureSecs = 0, passiveSecs = 0, idleSecs = 0, unknownSecs = 0;
    std::vector<Attention::Span> attention;
    std::string  action;
    std::string  actionLabel;
    std::string  resolution;
    // Effective focus seconds used for ranking, clustering, and
    // per-cluster theme weighting.  Defaults to totalFocusSecs but is
    // boosted by ApplyInferenceBoost() using the EntityStore's
    // short-horizon engagement percentile (bounded to <= 1.5x) so the dynamic context inference
    // pipeline agrees with the per-entity weighted store on which
    // entries matter.  Raw totalFocusSecs is left untouched so the
    // user-facing focus_seconds / dominant_focus_pct / items[].pct
    // JSON fields keep reporting the actual in-window dwell time.
    double       weightedFocusSecs = 0.0;
    int64_t      lastSeenTs     = 0;
    std::string  bestTitle;         // cleaned, ready for one-liner
    std::string  rawTitle;          // original window title before cleaning
    bool         isBrowser      = false;
    // Lookup key carriers for the EntityStore.  Populated only on
    // entries whose primary identity isn't the exe (file-activity virtuals
    // identify by file path; per-tab website virtuals identify by URL
    // when available).  Empty on regular focused-app entries -- those
    // fall back to lowercased(exePath).
    std::wstring filePath;          // populated for file-activity virts only
    std::wstring webUrl;            // populated for browsing virts only
    // Resolved entity identity (INTEL-1): the document / URL this entry is about when the
    // focus session resolved one, else empty (the entry is about the app itself).
    std::string  entityKey;
    std::string  entityType;        // file | url | ""
    std::wstring entityDisplay;
};

// =====================================================================
//  Semantic theme extraction.  Given the cleaned per-app titles in a
//  cluster, distill 1-2 *content* tokens that describe what the user is
//  doing -- not the verbatim title text.  No fixed taxonomy: the theme
//  emerges from the actual titles.  The granite sentence-encoder
//  (see Init) is used (when loaded) to weight tokens by semantic
//  centrality to the cluster.
// =====================================================================

// Stop-words: function words, generic UI / app verbs, anything that
// describes form rather than subject matter.  Lowercased.
static const std::unordered_set<std::string>& StopWords()
{
    static const std::unordered_set<std::string> s = {
        // articles, prepositions, conjunctions, pronouns
        "the","a","an","and","or","but","if","then","else","of","to","in",
        "on","at","by","for","with","from","up","out","over","under","into",
        "is","are","was","were","be","been","being","do","does","did","done",
        "have","has","had","this","that","these","those","my","your","his","her",
        "its","our","their","you","he","she","it","we","they","them","what",
        "which","who","whom","whose","when","where","why","how","all","any",
        "both","each","few","more","most","other","some","such","no","nor",
        "not","only","own","same","so","than","too","very","can","will","just",
        "should","now","also","i","me","mine","ours","yours","us","one","two",
        "three","four","five","six","seven","eight","nine","ten",
        // common low-information verbs / nouns that pollute themes
        "use","used","using","get","gets","got","getting","make","makes","made",
        "making","take","takes","took","taking","give","gave","given","giving",
        "want","wants","wanted","need","needs","needed","needing","like","likes",
        "liked","know","knows","knew","known","think","thinks","thought","see",
        "sees","saw","seen","feel","feels","felt","look","looks","looked","come",
        "comes","came","coming","going","gone","went","say","says","said","tell",
        "tells","told","ask","asks","asked","way","ways","time","times","day",
        "days","year","years","week","weeks","month","months","case","cases",
        "thing","things","good","bad","best","worst","great","nice","right",
        "wrong","first","last","next","prev","previous","old","new","big","small",
        "long","short","high","low","much","many","fast","slow","sure","real",
        "true","false","yes","another","still","yet","ever","never","always",
        "often","sometimes","maybe","really","actually","probably","perhaps",
        // url / web noise
        "com","net","org","io","co","www","http","https","html","htm",
        "page","pages","tab","window","url","link","links","site","sites","web",
        // file / document noise
        "file","files","new","untitled","draft","copy","backup","temp","tmp",
        "folder","folders","item","items","entry","entries",
        // generic action / category words
        "open","opened","close","closed","save","saved","print","share",
        "add","added","delete","remove","removed","update","updated",
        "view","viewing","read","reading","write","writing","edit","editing",
        "work","working","run","running","start","started","stop","stopped",
        "search","searching","find","finding","show","hide","help","about",
        "settings","menu","home","main","general","default","misc","other",
        "less","preview","details","intro","welcome","overview","summary",
        // months / days
        "jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec",
        "january","february","march","april","june","july","august","september",
        "october","november","december","mon","tue","wed","thu","fri","sat","sun",
        "monday","tuesday","wednesday","thursday","friday","saturday","sunday"
    };
    return s;
}

// File extensions / format names -- never useful as theme tokens.
static const std::unordered_set<std::string>& FileExts()
{
    static const std::unordered_set<std::string> s = {
        "cpp","hpp","cxx","cc","c","h","cs","java","kt","scala","go","rs",
        "py","rb","php","pl","sh","bash","ps1","bat","cmd",
        "js","jsx","ts","tsx","mjs","cjs","vue","svelte","html","htm","css",
        "scss","sass","less","yaml","yml","toml","ini","cfg","conf","json","xml",
        "md","markdown","txt","log","csv","tsv","tab","docx","doc","xlsx","xls",
        "pptx","ppt","pdf","png","jpg","jpeg","gif","bmp","ico","svg","webp",
        "mp4","mov","avi","mkv","mp3","wav","flac","zip","tar","gz","7z","rar",
        "exe","dll","msi","sys","obj","pdb","bin","dat","cache","lock"
    };
    return s;
}

// Brand / product / app names -- they describe the *vehicle*, not the
// *subject*, so they shouldn't appear in the semantic theme phrase.
static const std::unordered_set<std::string>& BrandNoise()
{
    static const std::unordered_set<std::string> s = {
        // Browsers + search / Q&A platforms
        "edge","chrome","firefox","safari","brave","opera","vivaldi",
        "mozilla","google","bing","yahoo","duckduckgo","ddg",
        "stackoverflow","stack","overflow","quora","reddit","wikipedia","wiki",
        // Microsoft ecosystem
        "microsoft","windows","msft","ms","office","outlook","teams","onedrive",
        "sharepoint","onenote","copilot","azure","msn",
        "word","excel","powerpoint","visio","project","loop",
        // Dev platforms
        "github","gitlab","bitbucket","azuredevops","ado","jira","confluence",
        "notion","atlassian",
        // Editors / IDEs
        "visual","studio","vscode","insiders","jetbrains","intellij","pycharm",
        "webstorm","clion","rider","goland","datagrip","resharper",
        "sublime","atom","emacs","vim","neovim","notepad","notepad++","npp",
        "explorer","terminal","powershell","cmd","wsl","cygwin","gitbash",
        // Comms
        "slack","zoom","skype","discord","whatsapp","telegram","signal","webex",
        // Streaming / media (rarely user "context", usually background)
        "youtube","spotify","netflix","disney","hulu","prime","amazon","ebay",
        "facebook","instagram","linkedin","twitter","x","tiktok","threads",
        "twitch","vimeo","soundcloud",
        // Misc ubiquitous
        "app","application","apps","desktop","mobile","beta","alpha","preview",
        "release","stable","portable","setup","installer","login","signin",
        "signup","sign","register","account","profile","user","admin",
        // Windows path-component noise.  These are kept tight: only
        // unambiguous Windows/Unix shell-folder names that almost never
        // form part of a real document/title content.  Generic words
        // like "documents", "downloads", "pictures", "music", "videos"
        // are intentionally NOT here -- they appear all the time in
        // legitimate file titles ("Quarterly Documents Review.docx",
        // "Photo Pictures from Trip.jpg", etc.).  The file-activity
        // augmentation now strictly allowlists extensions and skips
        // when a real file app has focus, so we don't need to nuke
        // these tokens at the theme layer too.
        "appdata","localdata","roaming","programdata","programs",
        "system32","syswow64",
        "temp","tmp","cache","caches","logs","dump","dumps",
        "default","userdata"
    };
    return s;
}

// Lowercase ASCII (UTF-8 multibyte preserved as-is).
static std::string AsciiLower(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back((c >= 'A' && c <= 'Z') ? char(c + 32) : char(c));
    return out;
}

// Capitalize the first ASCII letter of each word.
static std::string TitleCase(const std::string& s)
{
    std::string out = s;
    bool atStart = true;
    for (char& c : out)
    {
        unsigned char u = static_cast<unsigned char>(c);
        if (u <= ' ' || u == '-' || u == '_' || u == '/')
            atStart = true;
        else
        {
            if (atStart && u >= 'a' && u <= 'z')
                c = char(u - 32);
            atStart = false;
        }
    }
    return out;
}

// Split a raw token on camelCase / snake_case / digit boundaries and
// emit lowercase sub-tokens.
static void SplitWord(const std::string& w, std::vector<std::string>& out)
{
    std::string cur;
    auto flush = [&]() {
        if (cur.size() >= 3) out.push_back(AsciiLower(cur));
        cur.clear();
    };
    for (size_t i = 0; i < w.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(w[i]);
        if (c == '_' || c == '.' || c == '-' || c == '/' || c == '\\' ||
            c == ' ' || c == '\t' || c == '(' || c == ')' || c == '[' ||
            c == ']' || c == '{' || c == '}' || c == ':' || c == ';' ||
            c == '|' || c == '&' || c == '*' || c == '+' || c == '"' ||
            c == '\'' || c == ',' || c == '?' || c == '!' || c == '#' ||
            c == '@' || c == '<' || c == '>')
        {
            flush();
        }
        else if (cur.empty())
        {
            cur.push_back(char(c));
        }
        else
        {
            // camelCase boundary: prev lower -> cur upper
            unsigned char prev = static_cast<unsigned char>(cur.back());
            bool prevLower = (prev >= 'a' && prev <= 'z');
            bool curUpper  = (c    >= 'A' && c    <= 'Z');
            bool prevAlpha = (prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z');
            bool curDigit  = (c >= '0' && c <= '9');
            bool prevDigit = (prev >= '0' && prev <= '9');
            if ((prevLower && curUpper) || (prevAlpha && curDigit) || (prevDigit && !curDigit))
                flush();
            cur.push_back(char(c));
        }
    }
    flush();
}

// Tokenize a cleaned title into (lowercased, surface-form) pairs of
// content tokens.  Keeps a parallel "surface" form so the most common
// original casing can be recovered for display.
struct ContentTok
{
    std::string lower;
    std::string surface; // original casing of one occurrence
};

static std::vector<ContentTok>
ExtractContentTokens(const std::string& title)
{
    std::vector<ContentTok> out;
    if (title.empty()) return out;

    // First pass: split on whitespace + punctuation into "raw words"
    // (keeps original casing for surface form).
    std::vector<std::string> raw;
    std::string cur;
    for (size_t i = 0; i < title.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(title[i]);
        // High-bit bytes (UTF-8 continuation/lead) may belong to non-ASCII
        // tokens like " · ".  Treat them as separators so we don't pollute.
        if (c >= 0x80 || c <= ' ' || c == '-' || c == '_' || c == '.' ||
            c == '/' || c == '\\' || c == '(' || c == ')' || c == '[' ||
            c == ']' || c == '{' || c == '}' || c == ':' || c == ';' ||
            c == '|' || c == '&' || c == '*' || c == '+' || c == '"' ||
            c == '\'' || c == ',' || c == '?' || c == '!' || c == '#' ||
            c == '@' || c == '<' || c == '>')
        {
            if (!cur.empty()) { raw.push_back(cur); cur.clear(); }
        }
        else
        {
            cur.push_back(char(c));
        }
    }
    if (!cur.empty()) raw.push_back(cur);

    // Second pass: camelCase / digit splits + filter.
    const auto& stops  = StopWords();
    const auto& exts   = FileExts();
    const auto& brands = BrandNoise();

    for (const std::string& w : raw)
    {
        // Brand-aware: if the *whole* raw word matches a brand/stopword/
        // file-ext (case-insensitive), skip it entirely so we don't
        // produce sub-tokens like "Power Shell" from "PowerShell" or
        // "Tube" from "YouTube".
        std::string wLower = AsciiLower(w);
        if (brands.count(wLower) || stops.count(wLower) || exts.count(wLower))
            continue;

        std::vector<std::string> subs;
        SplitWord(w, subs);
        for (const std::string& lc : subs)
        {
            if (lc.size() < 3) continue;
            // Reject all-digits.
            bool allDigits = true;
            for (char ch : lc) if (ch < '0' || ch > '9') { allDigits = false; break; }
            if (allDigits) continue;
            if (stops.count(lc) || exts.count(lc) || brands.count(lc)) continue;

            // Recover an original-cased surface from `w` if possible
            // (case-insensitive substring match).
            std::string surface = lc;
            size_t pos = wLower.find(lc);
            if (pos != std::string::npos)
                surface = w.substr(pos, lc.size());

            out.push_back({ lc, surface });
        }
    }
    return out;
}

} // namespace

// =====================================================================
ContextInference::ContextInference()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

ContextInference::~ContextInference()
{
    Stop();

    delete m_ortSession;
    delete m_ortOpts;
    delete m_ortMemInfo;
    delete m_ortEnv;

    if (m_stopEvent) CloseHandle(m_stopEvent);
}

// =====================================================================
//  Init -- load the sentence-encoder model + tokenizer if available.
//
//  Sentence-encoder loader.
//
//  Single supported model: ibm-granite/granite-embedding-small-english-r2
//  (ModernBERT, byte-level BPE, 384-dim, code-aware) under
//  <modelsDir>\granite\.  Expected files:
//      model_quantized.onnx (+ model_quantized.onnx_data)
//      vocab.txt              \
//      merges.txt              > produced at CI time from
//      special_tokens.txt     /  tokenizer.json by
//                             scripts/extract_modernbert_tokenizer.py
//
//  Best-effort: if the granite files are missing or invalid the engine
//  still runs in deterministic-only mode (m_modelReady=false), with
//  the algorithmic cluster->theme pipeline producing summaries using
//  pure token-frequency / focus-weighted scoring -- no semantic
//  embeddings.  Older BGE-small / MiniLM fallback paths were removed
//  in v5.16 since the production stack is granite + qwen exclusively.
// =====================================================================
bool ContextInference::Init(const std::wstring& modelsDir)
{
    if (modelsDir.empty()) return true;  // explicitly skip model load

    std::wstring graniteDir = modelsDir + L"\\granite";
    std::wstring modelPath  = graniteDir + L"\\model_quantized.onnx";
    std::string  vocabPath  = WideToUtf8(graniteDir) + "\\vocab.txt";
    std::string  mergesPath = WideToUtf8(graniteDir) + "\\merges.txt";
    std::string  specPath   = WideToUtf8(graniteDir) + "\\special_tokens.txt";

    if (GetFileAttributesW(modelPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return true;   // graceful degrade -- deterministic mode
    if (!m_mbTokenizer.Load(vocabPath, mergesPath, specPath))
        return true;   // tokenizer files missing or malformed

    try
    {
        m_ortEnv  = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "WarpContextInference");
        m_ortOpts = new Ort::SessionOptions();
        m_ortOpts->SetIntraOpNumThreads(2);
        m_ortOpts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        m_ortSession = new Ort::Session(*m_ortEnv, modelPath.c_str(), *m_ortOpts);
        m_ortMemInfo = new Ort::MemoryInfo(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));
    }
    catch (const Ort::Exception&)
    {
        delete m_ortSession; m_ortSession = nullptr;
        delete m_ortOpts;    m_ortOpts    = nullptr;
        delete m_ortMemInfo; m_ortMemInfo = nullptr;
        delete m_ortEnv;     m_ortEnv     = nullptr;
        return true;  // graceful degrade
    }

    m_modelReady = true;
    m_modelName  = "granite-embedding-small-english-r2";
    return true;
}

// =====================================================================
//  Sentence embedding (384-dim, L2-normalized).
//
//  granite (ModernBERT) topology: inputs `input_ids` + `attention_mask`,
//  output `sentence_embedding` -- the model already mean-pools and we
//  just L2-normalize.
//
//  Empty vector if the model isn't loaded or inference fails.
// =====================================================================
std::vector<float> ContextInference::Embed(const std::string& text)
{
    if (!m_modelReady || !m_ortSession || text.empty()) return {};
    {
        std::lock_guard<std::mutex> lk(m_embedMtx);
        auto it = m_embedCache.find(text);
        if (it != m_embedCache.end()) return it->second;
    }
    std::vector<float> v = EmbedUncached(text);
    if (v.empty()) return v;
    std::lock_guard<std::mutex> lk(m_embedMtx);
    if (m_embedCache.emplace(text, v).second)
    {
        m_embedOrder.push_back(text);
        while (m_embedOrder.size() > 1024)
        {
            m_embedCache.erase(m_embedOrder.front());
            m_embedOrder.pop_front();
        }
    }
    return v;
}

std::vector<float> ContextInference::EmbedUncached(const std::string& text)
{
    if (!m_modelReady || !m_ortSession) return {};

    std::vector<int64_t> inputIds =
        m_mbTokenizer.Encode(text, MAX_SEQ_LEN);
    if (inputIds.empty()) return {};
    std::vector<int64_t> attentionMask(inputIds.size(), 1);

    const int64_t seqLen = static_cast<int64_t>(inputIds.size());
    std::array<int64_t, 2> shape = { 1, seqLen };

    Ort::Value inputIdsTensor = Ort::Value::CreateTensor<int64_t>(
        *m_ortMemInfo, inputIds.data(), inputIds.size(),
        shape.data(), shape.size());
    Ort::Value attMaskTensor = Ort::Value::CreateTensor<int64_t>(
        *m_ortMemInfo, attentionMask.data(), attentionMask.size(),
        shape.data(), shape.size());

    const char* inputNames[]  = { "input_ids", "attention_mask" };
    const char* outputNames[] = { "sentence_embedding" };

    std::vector<Ort::Value> inputTensors;
    inputTensors.push_back(std::move(inputIdsTensor));
    inputTensors.push_back(std::move(attMaskTensor));

    std::vector<Ort::Value> outputTensors;
    try
    {
        outputTensors = m_ortSession->Run(
            Ort::RunOptions{ nullptr },
            inputNames, inputTensors.data(), 2,
            outputNames, 1);
    }
    catch (const Ort::Exception&)
    {
        return {};
    }

    const float* raw = outputTensors[0].GetTensorData<float>();
    std::vector<float> embedding(raw, raw + EMBED_DIM);

    float norm = 0.0f;
    for (int d = 0; d < EMBED_DIM; ++d) norm += embedding[d] * embedding[d];
    norm = std::sqrt(norm);
    if (norm > 1e-9f)
    {
        for (int d = 0; d < EMBED_DIM; ++d) embedding[d] /= norm;
    }
    return embedding;
}

float ContextInference::CosineSim(const std::vector<float>& a,
                                  const std::vector<float>& b)
{
    if (a.size() != b.size() || a.empty()) return 0.0f;
    float dot = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
    return dot;
}

void ContextInference::Start(ActivityStore* store, FocusTracker* fg)
{
    if (m_running) return;
    m_store = store;
    m_fg = fg;
    m_running = true;
    ResetEvent(m_stopEvent);
    m_thread = std::thread(&ContextInference::TimerLoop, this);
}

void ContextInference::SetLlmSummarizer(LlmSummarizer* llm)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_llm = llm;
}

void ContextInference::SetEntityStore(EntityStore* es)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_entities = es;
}

void ContextInference::SetHistoryEngine(HistoryEngine* h)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_historyEngine = h;
}

void ContextInference::SetPresenceProvider(std::function<std::string()> provider)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_presence = std::move(provider);
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
    SetThreadDescription(GetCurrentThread(), L"WARP context");
    // Background inference is never latency-critical: let Windows schedule it on efficient
    // cores at low frequency (EcoQoS, PERF-2).
    THREAD_POWER_THROTTLING_STATE pt = {};
    pt.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &pt, sizeof(pt));

    // Identity of what is in front right now (empty while away / nothing tracked).
    auto focusSignature = [this]() -> std::wstring
    {
        FocusTracker::ActiveSession a;
        if (m_fg && m_fg->GetCurrentSession(a)) return a.exePath + L"\x1f" + a.windowTitle;
        return std::wstring();
    };

    std::wstring lastFg = focusSignature();
    RunOnce();
    // Read AFTER each compose: RunOnce's own work-thread write (HistoryEngine::PersistThread) advances
    // the change sequence and must not count as new activity, or no cycle would ever be skipped.
    int64_t lastSeq = m_store ? m_store->CurrentChangeSeq() : 0;
    ULONGLONG lastRunTick = GetTickCount64();
    while (m_running)
    {
        int cadence = Settings::Instance().Get().contextCadenceSeconds;
        if (cadence < 15) cadence = 15;
        if (cadence > 3600) cadence = 3600;
        // PERF-2: half as often on battery or with battery saver on.
        SYSTEM_POWER_STATUS power = {};
        if (GetSystemPowerStatus(&power) && (power.ACLineStatus == 0 || power.SystemStatusFlag == 1))
            cadence = (std::min)(cadence * 2, 3600);
        DWORD r = WaitForSingleObject(m_stopEvent, (DWORD)cadence * 1000);
        if (r == WAIT_OBJECT_0 || !m_running) break;
        // PERF-1: the optional refiner does not stay resident while unused.
        LlmSummarizer* llm = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            llm = m_llm;
        }
        if (llm) llm->UnloadIfIdle(10 * 60 * 1000);
        if (Settings::Instance().IsPaused(static_cast<int64_t>(std::time(nullptr)))) continue;
        // PERF-2: nothing new committed and the same window in front => the context has not
        // changed materially; refresh at most every 5 minutes (in-progress dwell still grows).
        const int64_t seq = m_store ? m_store->CurrentChangeSeq() : 0;
        const std::wstring fg = focusSignature();
        if (seq == lastSeq && fg == lastFg && GetTickCount64() - lastRunTick < 5ull * 60 * 1000) continue;
        RunOnce();
        lastSeq = m_store ? m_store->CurrentChangeSeq() : 0;
        lastFg = fg;
        lastRunTick = GetTickCount64();
    }
}

void ContextInference::RunOnce()
{
    if (!m_store || !m_store->IsOpen()) return;
    const uint64_t epoch = m_epoch.load();
    uint64_t histGeneration = UINT64_MAX;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_historyEngine) histGeneration = m_historyEngine->Generation();   // before any input is read
    }

    ContextSnapshot snap;
    try
    {
        snap = ComposeSnapshot(0, true);
    }
    catch (const std::exception& e)
    {
        WARP_LOG_ERROR("context", "compose failed: %s", e.what());
        return;
    }

    HistoryEngine* hist = nullptr;
    bool append = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_epoch.load() != epoch) return;          // history was cleared while composing
        append = ShouldAppendToHistory(snap);
        hist = m_historyEngine;
    }

    // Record the observation in the historical layer (thread assignment every cycle;
    // persistence only on material change).
    if (hist)
    {
        SnapshotInput in;
        in.ts = snap.timestamp;
        in.windowSecs = snap.windowSeconds;
        for (const auto& e : snap.entities)
        {
            SnapshotEntity se;
            se.entityKey = e.entityKey;
            se.entityType = e.entityType;
            se.display = WarpJson::Utf8ToWide(e.display);
            se.activeSecs = e.activeSecs;
            in.entities.push_back(std::move(se));
        }
        in.label = snap.summary.empty() ? std::string() : snap.summary.front();
        if (!in.label.empty()) in.embedding = Embed(in.label);
        in.model = snap.model;
        in.persist = append;
        in.generation = histGeneration;
        if (append) in.json = SnapshotToJsonObject(snap, "all", false);
        if (m_epoch.load() != epoch) return;          // cleared while embedding: never persist pre-clear activity
        snap.threadId = hist->RecordSnapshot(in);
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_epoch.load() != epoch) return;
    m_latest     = snap;
    m_haveLatest = true;

    if (append)
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

    // Append on material change: different one-liner (combined or any of
    // the per-category lines) OR different dominant app.
    if (snap.summary         != prev.summary)         return true;
    if (snap.summaryFiles    != prev.summaryFiles)    return true;
    if (snap.summaryWebsites != prev.summaryWebsites) return true;
    if (snap.summaryApps     != prev.summaryApps)     return true;
    if (!snap.items.empty() && !prev.items.empty()
     && snap.items.front().exe != prev.items.front().exe)
        return true;

    return false;
}

// =====================================================================
//  Snapshot composition
// =====================================================================

// ApplyInferenceBoost
// ---------------------------------------------------------------------
// Bias each entry's `weightedFocusSecs` by the entity's historical engagement from the
// EntityStore, as a BOUNDED tie-breaker (INTEL-4):
//
//     weightedFocusSecs = totalFocusSecs * (1 + 0.5 * shortHorizonPercentile)   // 1.0x .. 1.5x
//
// The previous unbounded formula (1 + ln(1+recency/50) + ln(1+open7d/5), up to ~5x) let
// habitual apps (mail, chat, the browser) float above what the user is doing right now.
// With a 1.5x ceiling, 10 focused minutes always beat 6 minutes on a "popular" entity,
// while equally-focused entries are ordered by how much they matter to the user.
// Multiplicative: zero in-window focus stays zero (history never invents presence).
static void ApplyInferenceBoost(std::vector<AppAgg>&                 bag,
                                EntityStore*                         es,
                                std::function<std::string(const AppAgg&)> keyOf, int64_t asOf)
{
    for (auto& a : bag)
        a.weightedFocusSecs = static_cast<double>(a.totalFocusSecs);
    if (!es || bag.empty()) return;

    std::vector<std::string> keys;
    std::vector<size_t> keyToIdx;
    keys.reserve(bag.size());
    keyToIdx.reserve(bag.size());
    for (size_t i = 0; i < bag.size(); ++i)
    {
        std::string k = keyOf(bag[i]);
        if (k.empty()) continue;
        keys.push_back(std::move(k));
        keyToIdx.push_back(i);
    }
    if (keys.empty()) return;

    std::vector<EntityScore> scores = es->Score(keys, "short", asOf, false);
    for (size_t i = 0; i < scores.size() && i < keyToIdx.size(); ++i)
    {
        if (!scores[i].found) continue;
        AppAgg& a = bag[keyToIdx[i]];
        a.weightedFocusSecs = static_cast<double>(a.totalFocusSecs) * (1.0 + 0.5 * scores[i].score);
    }
}

// Entity key used for boosting / joining: the resolved document / URL when known, else the app.
static std::string EntityKeyOfAgg(const AppAgg& a)
{
    if (!a.entityKey.empty()) return a.entityKey;
    if (!a.filePath.empty()) return EntityKey::FileKey(a.filePath);
    if (!a.webUrl.empty()) return EntityStore::NormalizeKey(a.webUrl);
    if (!a.exePath.empty() || !a.aumid.empty()) return StoreUtil::AppEntityKey(a.exePath, a.aumid);
    return std::string();
}

// =====================================================================
//  Rich deterministic listing (v5.17)
//
//  Empirical finding (local A/B harness on Qwen3-0.6B + granite over
//  12 realistic scenarios -- see commit message): a 0.6B on-device LLM
//  cannot ABSTRACT diverse window titles into a higher-level intent
//  ("auth_middleware.cpp + login.ts + session.go" -> "authentication
//  system").  All it can do is rephrase / list the titles -- and it
//  does so LESS completely (drops items) and LESS reliably (its output
//  is rejected by the grounding gate 75-90% of the time, so the user
//  is usually left looking at the terse 1-token theme).
//
//  A deterministic listing of the top cleaned title phrases is, on the
//  same granite metric the product itself uses, MORE grounded
//  (cos-to-titles +0.05), closer to a human reference (ROUGE-L +0.20,
//  +80% relative) and ~2x more specific than the token-theme -- at
//  ZERO inference cost and full determinism.  It also matches the
//  expensive best-of-N LLM on semantic similarity to an ideal summary.
//
//  This composes "User is <verb> <p1>, <p2> and <p3><app clause>"
//  from the top distinct cleaned title phrases in the ranked bag.
//  Returns `themeHint` unchanged when the bag yields no usable phrase
//  (so we never regress below the prior behaviour for empty input).
// =====================================================================
namespace
{
    // Strip a recognized trailing file extension from a phrase.
    std::string StripTrailingExt(const std::string& s)
    {
        static const std::unordered_set<std::string> kExt = {
            "cpp","h","hpp","cc","cxx","cs","java","kt","py","rb","pl",
            "php","js","mjs","cjs","jsx","ts","tsx","go","rs","swift",
            "m","mm","dart","sh","bash","zsh","ps1","psm1","html","htm",
            "css","scss","sass","less","xml","json","yaml","yml","toml",
            "ini","cfg","conf","sql","graphql","vue","svelte","r","jl",
            "lua","ex","exs","erl","hs","clj","docx","doc","xlsx","xls",
            "pptx","ppt","pdf","txt","md","rst","tex","one","csv","tsv",
            "sln","vcxproj","png","jpg","jpeg","gif","svg"
        };
        size_t dot = s.find_last_of('.');
        if (dot == std::string::npos || dot == 0) return s;
        std::string ext = AsciiLower(s.substr(dot + 1));
        if (kExt.count(ext)) return s.substr(0, dot);
        return s;
    }

    // Turn a (already app-suffix-cleaned) window title into a short,
    // human-readable phrase: strip extension, snake/kebab/camel ->
    // spaces, drop leading "Re:"/"#"/article and dangling trailing
    // connector/version tokens, cap at 4 words.
    std::string CleanPhrase(const std::string& title, size_t maxWords = 4)
    {
        std::string t = StripTrailingExt(title);

        // snake / kebab -> space; camelCase -> spaced
        std::string spaced;
        spaced.reserve(t.size() + 8);
        for (size_t i = 0; i < t.size(); ++i)
        {
            char c = t[i];
            if (c == '_' || c == '-' || c == '/' || c == '\\')
            {
                spaced.push_back(' ');
            }
            else
            {
                // insert a space at a lower->Upper camelCase boundary
                if (i > 0 && c >= 'A' && c <= 'Z'
                    && t[i - 1] >= 'a' && t[i - 1] <= 'z')
                    spaced.push_back(' ');
                spaced.push_back(c);
            }
        }

        // strip a leading "re:" / "fwd:" / "fw:" reply/forward marker
        {
            std::string lo = AsciiLower(spaced);
            static const char* const kLead[] = { "re:", "fwd:", "fw:" };
            for (const char* m : kLead)
            {
                size_t ml = std::strlen(m);
                if (lo.size() >= ml && lo.compare(0, ml, m) == 0)
                {
                    spaced = spaced.substr(ml);
                    break;
                }
            }
        }

        // strip leading punctuation noise (#, @, *, :, >, |, -, spaces)
        {
            size_t p = 0;
            while (p < spaced.size())
            {
                char c = spaced[p];
                if (c == '#' || c == '@' || c == '*' || c == ':' ||
                    c == '>' || c == '|' || c == '-' || c == ' ')
                    ++p;
                else
                    break;
            }
            if (p > 0) spaced = spaced.substr(p);
        }

        // tokenize on whitespace, dropping leading punctuation noise
        std::vector<std::string> words;
        {
            std::string cur;
            for (char c : spaced)
            {
                unsigned char u = static_cast<unsigned char>(c);
                if (u <= ' ')
                {
                    if (!cur.empty()) { words.push_back(cur); cur.clear(); }
                }
                else
                {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) words.push_back(cur);
        }

        // strip leading punctuation-only / article tokens
        static const std::unordered_set<std::string> kLeadDrop = {
            "a", "an", "the"
        };
        while (!words.empty())
        {
            std::string w = words.front();
            // strip surrounding punctuation
            std::string stripped;
            for (char c : w)
            {
                unsigned char u = static_cast<unsigned char>(c);
                if (std::isalnum(u)) stripped.push_back(c);
                else if (!stripped.empty()) break;
            }
            if (stripped.empty() || kLeadDrop.count(AsciiLower(stripped)))
                words.erase(words.begin());
            else
                break;
        }

        if (words.size() > maxWords) words.resize(maxWords);

        // drop dangling trailing connector / version tokens
        static const std::unordered_set<std::string> kDangle = {
            "to","the","a","an","of","for","in","on","and","or","with",
            "from","f","vs","ch","p99","v1","v2","v3","v4","v5","draft",
            "is","at","by"
        };
        while (!words.empty() && kDangle.count(AsciiLower(words.back())))
            words.pop_back();

        std::string out;
        for (size_t i = 0; i < words.size(); ++i)
        {
            if (i) out += " ";
            out += words[i];
        }
        return Trim(out);
    }
}

static std::vector<std::string> ComposeRichListing(
    const std::vector<AppAgg>&       bag,
    const std::string&               category,
    const std::vector<std::string>&  themeHint)
{
    if (bag.empty()) return themeHint;

    // Collect up to 3 distinct cleaned title phrases, in ranked order.
    std::vector<std::string> phrases;
    std::vector<std::string> phrasesLower;
    for (const auto& a : bag)
    {
        const std::string& src = !a.bestTitle.empty() ? a.bestTitle : a.rawTitle;
        std::string p = CleanPhrase(src);
        if (p.size() < 2) continue;
        std::string lo = AsciiLower(p);

        // skip if this phrase is contained in, or contains, one already
        // taken (avoids "Search Relevance" + "Search Relevance Spec").
        bool dup = false;
        for (const auto& seen : phrasesLower)
        {
            if (lo.find(seen) != std::string::npos ||
                seen.find(lo) != std::string::npos)
            { dup = true; break; }
        }
        if (dup) continue;

        phrases.push_back(std::move(p));
        phrasesLower.push_back(std::move(lo));
        if (phrases.size() >= 3) break;
    }

    if (phrases.empty()) return themeHint;

    const char* verb =
        (category == "websites") ? "researching" :
        (category == "apps")     ? "working across" :
                                   "working on";

    std::string body;
    if (phrases.size() == 1)
        body = phrases[0];
    else if (phrases.size() == 2)
        body = phrases[0] + " and " + phrases[1];
    else
        body = phrases[0] + ", " + phrases[1] + " and " + phrases[2];

    // Brief app clause: up to 2 distinct friendly names.
    std::vector<std::string> apps;
    for (const auto& a : bag)
    {
        if (a.friendlyName.empty()) continue;
        bool seen = false;
        for (const auto& x : apps) if (x == a.friendlyName) { seen = true; break; }
        if (!seen) apps.push_back(a.friendlyName);
        if (apps.size() >= 2) break;
    }
    std::string appClause;
    if (apps.size() == 1)
        appClause = " in " + apps[0];
    else if (apps.size() >= 2)
        appClause = " across " + apps[0] + " and " + apps[1];

    return { "User is " + std::string(verb) + " " + body + appClause };
}

ContextSnapshot ContextInference::ComposeSnapshot(int64_t windowSecs, bool allowLlm, int64_t asOf)
{
    if (windowSecs <= 0) windowSecs = CONTEXT_WINDOW_SECS;

    ContextSnapshot snap;
    snap.timestamp     = asOf > 0 ? asOf : static_cast<int64_t>(std::time(nullptr));
    snap.windowEndTs   = snap.timestamp;
    snap.windowStartTs = snap.timestamp - windowSecs;
    snap.windowSeconds = windowSecs;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_presence) snap.presence = m_presence();
    }

    // ---- Gather raw signals (bounded reads, INTEL-7) ----------------
    // Focus sessions are intervals: take every session OVERLAPPING the window and clip it,
    // scaling its active (non-idle) seconds to the in-window part (INTEL-3). A 3-hour session
    // that ended 14 minutes ago therefore contributes 60 s to a 15-minute window, not 10,800 s.
    const int kMaxRows = 20000;
    auto focus  = m_store->QueryFocus(snap.windowStartTs, snap.windowEndTs, kMaxRows);
    auto browse = m_store->QueryBrowsing(snap.windowStartTs, snap.windowEndTs, kMaxRows);
    auto apps   = m_store->QueryLaunches(snap.windowStartTs, snap.windowEndTs, kMaxRows);
    auto files  = m_store->QueryFiles(snap.windowStartTs, snap.windowEndTs, kMaxRows);
    for (auto& f : focus)
    {
        int64_t s = (std::max)(f.startTs, snap.windowStartTs);
        int64_t e = (std::min)(f.endTs > 0 ? f.endTs : f.timestampUtc, snap.windowEndTs);
        f.activeSecs = Attention::Clip(f.activeSecs, f.startTs, f.endTs, s, e);
        f.passiveSecs = Attention::Clip(f.passiveSecs, f.startTs, f.endTs, s, e);
        f.idleSecs = Attention::Clip(f.idleSecs, f.startTs, f.endTs, s, e);
        f.unknownSecs = Attention::Clip(f.unknownSecs, f.startTs, f.endTs, s, e);
        f.durationSecs = (int)(std::max)(int64_t(0), e - s);
        f.startTs = s;
        f.endTs = e;
        f.timestampUtc = e;
    }
    // A deleted / renamed-away file is not "being worked on" (INTEL-1).
    files.erase(std::remove_if(files.begin(), files.end(), [](const FileActivity& f) {
        return f.isDir || f.action == L"DELETE" || f.action == L"RENAME" || f.confidence < 0.2;
    }), files.end());

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
    // FocusTracker only emits a row when a session ends.  Ask it for the session in
    // progress (with its resolved document) and add the in-window part as a virtual row.
    if (m_fg)
    {
        FocusTracker::ActiveSession active;
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
                virt.timestampUtc = snap.timestamp;
                virt.startTs      = effectiveStart;
                virt.endTs        = snap.timestamp;
                virt.exeName      = active.exeName;
                virt.exePath      = active.exePath;
                virt.windowTitle  = active.windowTitle;
                virt.durationSecs = dwellInWindow;
                virt.activeSecs   = Attention::Clip(active.activeSecsSoFar, startTs, snap.timestamp, effectiveStart, snap.timestamp);
                virt.passiveSecs  = Attention::Clip(active.passiveSecsSoFar, startTs, snap.timestamp, effectiveStart, snap.timestamp);
                virt.idleSecs     = Attention::Clip(active.idleSecsSoFar, startTs, snap.timestamp, effectiveStart, snap.timestamp);
                virt.unknownSecs  = Attention::Clip(active.unknownSecsSoFar, startTs, snap.timestamp, effectiveStart, snap.timestamp);
                virt.aumid        = active.aumid;
                virt.docResolution = active.docResolution;
                virt.docKey       = active.docKey;
                virt.docDisplay   = active.docDisplay;
                virt.docType      = active.docType;
                focus.push_back(virt);
                if (std::find(snap.signalTypes.begin(), snap.signalTypes.end(), "app_focus") == snap.signalTypes.end())
                {
                    snap.signalTypes.push_back("app_focus");
                    std::sort(snap.signalTypes.begin(), snap.signalTypes.end());
                }
                snap.activityCount += 1;
            }
        }
    }

    // ---- Aggregate by (app, document) -----------------------------
    // INTEL-1: the unit is the document / URL the focus session resolved to, not the exe, so
    // five files edited in VS Code are five entries with their own dwell time.
    std::unordered_map<std::string, AppAgg> byExe; // key: exe basename [+ "|" + entity key]

    int totalFocusSecs = 0;

    for (const auto& f : focus)
    {
        if (f.durationSecs <= 0) continue;
        std::wstring exeLower  = ToLower(f.exeName);
        std::wstring pathLower = ToLower(f.exePath);
        if (IsBoringExe(exeLower)) continue;

        std::string key = StoreUtil::AppEntityKey(f.exePath, f.aumid);
        if (key.empty()) key = ExeBasenameUtf8(exeLower);
        if (!f.docKey.empty()) key += "|" + f.docKey;
        auto& agg = byExe[key];
        if (agg.exeName.empty())
        {
            agg.exeName = f.exeName;
            agg.exePath = f.exePath;
            agg.aumid = f.aumid;
            auto cls = ClassifyApp(f.exeName, f.exePath);
            agg.friendlyName = cls.first;
            agg.verb         = cls.second;
            agg.isBrowser    = IsBrowser(exeLower, pathLower);
            agg.entityKey    = f.docKey;
            agg.entityType   = f.docType == "file" ? "file" : (f.docKey.empty() ? "" : "url");
            agg.entityDisplay = f.docDisplay;
            agg.resolution = f.docResolution;
            if (agg.entityType == "url") agg.webUrl = f.docDisplay;
            else if (agg.entityType == "file") agg.filePath = f.docDisplay;
        }
        const double wall = (double)(std::max)(1, f.durationSecs);
        agg.attention.push_back({ f.startTs, f.endTs, (std::max)(0, f.activeSecs) / wall,
                                                     (std::max)(0, f.passiveSecs) / wall });
        agg.idleSecs += (std::max)(0, f.idleSecs);
        if (f.timestampUtc > agg.lastSeenTs)
        {
            agg.lastSeenTs = f.timestampUtc;
            std::string title = WideToUtf8(f.windowTitle);
            agg.rawTitle  = title;
            agg.bestTitle = CleanTitle(title, agg.friendlyName);
        }
    }

    std::vector<Attention::Span> allAttention;
    for (auto& pair : byExe)
    {
        auto& a = pair.second;
        double active = 0, passive = 0;
        a.attention = Attention::Union(a.attention);
        for (const auto& s : a.attention)
        {
            const int wall = (int)(s.end - s.start);
            a.exposureSecs += wall;
            active += wall * s.active;
            passive += wall * s.passive;
            allAttention.push_back(s);
        }
        a.totalFocusSecs = (int)std::lround(active);
        a.passiveSecs = (std::min)(a.exposureSecs - a.totalFocusSecs, (int)std::lround(passive));
        a.idleSecs = (std::min)(a.idleSecs, a.exposureSecs - a.totalFocusSecs - a.passiveSecs);
        a.unknownSecs = a.exposureSecs - a.totalFocusSecs - a.passiveSecs - a.idleSecs;
        totalFocusSecs += a.totalFocusSecs;
        snap.idleSeconds += a.idleSecs;
    }
    double unionActive = 0, unionPassive = 0;
    for (const auto& s : Attention::Union(allAttention))
    {
        const int wall = (int)(s.end - s.start);
        snap.exposureSeconds += wall;
        unionActive += wall * s.active;
        unionPassive += wall * s.passive;
    }
    snap.focusSeconds = (int)std::lround(unionActive);
    snap.passiveSeconds = (std::min)(snap.exposureSeconds - snap.focusSeconds, (int)std::lround(unionPassive));
    snap.idleSeconds = (std::min)(snap.idleSeconds, snap.exposureSeconds - snap.focusSeconds - snap.passiveSeconds);
    snap.unknownSeconds = snap.exposureSeconds - snap.focusSeconds - snap.passiveSeconds - snap.idleSeconds;

    // Sparse windows still have observations, but they have no measured focus seconds.
    // Use typed actions rather than inventing duration or turning an OPEN into an edit.
    if (totalFocusSecs == 0)
    {
        auto add = [&](AppAgg a) {
            const std::string key = EntityKeyOfAgg(a);
            if (!key.empty()) for (auto& existing : byExe)
            {
                if (EntityKeyOfAgg(existing.second) != key) continue;
                if (a.lastSeenTs >= existing.second.lastSeenTs)
                {
                    if (existing.second.exposureSecs == 0) existing.second = std::move(a);
                    else
                    {
                        existing.second.action = a.action;
                        existing.second.actionLabel = a.actionLabel;
                        existing.second.lastSeenTs = a.lastSeenTs;
                    }
                }
                return;
            }
            const std::string grouping = key.empty() ? a.action + "|" + a.rawTitle : key;
            auto it = byExe.find(grouping);
            if (it == byExe.end() || a.lastSeenTs > it->second.lastSeenTs) byExe[grouping] = std::move(a);
        };
        for (const auto& f : files)
        {
            if (EntityStore::IsTransientFileName(f.path)) continue;
            AppAgg a;
            a.entityKey = f.entityKey;
            a.entityType = "file";
            a.entityDisplay = a.filePath = f.path;
            a.rawTitle = a.bestTitle = WideToUtf8(EntityKey::BaseName(f.path));
            a.lastSeenTs = f.timestampUtc;
            a.action = AsciiLower(WideToUtf8(f.action));
            a.actionLabel = f.action == L"OPEN" ? "Opened" :
                f.action == L"DOWNLOAD" ? "Download observed" :
                f.action == L"CREATE" ? "Creation observed" : "Modification observed";
            a.resolution = "observed_path";
            add(std::move(a));
        }
        for (const auto& b : browse)
        {
            if (b.confidence < 0.2) continue;
            AppAgg a;
            a.entityKey = b.url.empty() && b.docId.empty() ? "" : b.entityKey;
            a.entityType = "url";
            a.entityDisplay = a.webUrl = b.url;
            a.rawTitle = a.bestTitle = WideToUtf8(b.title.empty() ? b.url : b.title);
            a.friendlyName = WideToUtf8(b.browser);
            a.isBrowser = true;
            a.lastSeenTs = b.timestampUtc;
            a.action = "page_observed";
            a.actionLabel = "Page observed";
            a.resolution = a.entityKey.empty() ? "unresolved" : "browser_window";
            add(std::move(a));
        }
        for (const auto& app : apps)
        {
            if (app.confidence < 0.2 || IsBoringExe(ToLower(app.exeName))) continue;
            AppAgg a;
            a.exeName = app.exeName;
            a.exePath = app.exePath;
            a.aumid = app.aumid;
            a.friendlyName = ClassifyApp(app.exeName, app.exePath).first;
            a.rawTitle = a.bestTitle = a.friendlyName;
            a.entityType = "app";
            a.entityDisplay = app.aumid.empty() ? app.exePath : app.aumid;
            a.lastSeenTs = app.timestampUtc;
            a.action = "launch";
            a.actionLabel = "Launch observed";
            a.resolution = "process";
            add(std::move(a));
        }
    }

    // ---- Score & rank apps ----------------------------------------
    std::vector<AppAgg> ranked;
    ranked.reserve(byExe.size());
    for (auto& kv : byExe) ranked.push_back(std::move(kv.second));

    // Bounded historical tie-breaker (INTEL-4), keyed by the resolved entity when known.
    EntityStore* entities = nullptr;
    LlmSummarizer* llm = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        entities = m_entities;
        llm = m_llm;
    }
    ApplyInferenceBoost(ranked, entities, [](const AppAgg& a) { return EntityKeyOfAgg(a); }, snap.timestamp);

    std::sort(ranked.begin(), ranked.end(),
              [](const AppAgg& a, const AppAgg& b){
                  if (a.weightedFocusSecs != b.weightedFocusSecs)
                      return a.weightedFocusSecs > b.weightedFocusSecs;
                  if (a.lastSeenTs != b.lastSeenTs) return a.lastSeenTs > b.lastSeenTs;
                  return EntityKeyOfAgg(a) < EntityKeyOfAgg(b);
              });

    // dominant_focus_pct stays based on RAW totalFocusSecs -- the
    // boost is an internal ranking signal, never a user-facing
    // percentage.  Otherwise "100% of focus" would lose its meaning
    // when a single boosted entry crosses the in-window total.
    if (!ranked.empty() && totalFocusSecs > 0)
        snap.dominantPct = static_cast<int>(
            (ranked.front().totalFocusSecs * 100LL) / totalFocusSecs);

    // ---- One-liner composition ------------------------------------
    auto phraseFor = [](const AppAgg& a) -> std::string {
        if (a.bestTitle.empty())
            return std::string(a.verb) + " " + a.friendlyName;
        return std::string(a.verb) + " \"" + a.bestTitle + "\" in " + a.friendlyName;
    };

    // -----------------------------------------------------------------
    // Composable one-liner builder.  Takes a bag of AppAgg entries (any
    // projection of the activity stream -- the full ranked list, the
    // doc-editor subset, the per-tab browsing list, etc.) and produces
    // the same `<verb> <Theme>` one-liner using the existing sentence-encoder
    // clustering + theme-distillation pipeline.  Used four times below
    // to build the combined one-liner plus the three per-category ones
    // (Documents / Websites / Apps).
    //
    // Returns the rendered text PLUS the per-entry cluster assignments
    // and the cluster count, so the caller can use them to populate
    // items[].threadId for the "All" projection (the only projection
    // surfaced through items[]).
    // -----------------------------------------------------------------
    struct OneLinerBuild
    {
        std::vector<std::string> lines;
        std::vector<int>         clusterOf;     // per-bag-entry cluster id, 0-based
        int                      threadCount = 0;
    };

    auto composeOneLinerFromBag = [&](const std::vector<AppAgg>& bag,
                                       bool categoryMode = false) -> OneLinerBuild {
        OneLinerBuild out;
        if (bag.empty()) return out;

        // ---- Cluster the bag -----------------------------------------
        std::vector<int>                 clOf(bag.size(), -1);
        std::vector<std::vector<float>>  centroids;
        std::vector<double>              clTotalFocus;
        std::vector<std::vector<size_t>> clMembers;
        double                           bagTotalFocus = 0.0;
        for (const auto& a : bag) bagTotalFocus += (std::max)(0.0, a.weightedFocusSecs);

        if (m_modelReady && bag.size() >= 2)
        {
            // Natural clustering for both "All" and category modes.
            // The old categoryMode behaviour forced every bag entry
            // into one virtual cluster as a workaround for the small/
            // diverse bag -> singleton-cluster -> verbatim-mish-mash
            // failure mode -- which is now handled by focus-weighted
            // theme scoring (low-focus titles' tokens get demoted to
            // 0.2x score below 5% focus coverage).  Natural clusters
            // give each meaningful sub-thread its own summary line.
            const size_t embedLimit = (std::min)(bag.size(), static_cast<size_t>(8));
            std::vector<std::vector<float>> embs(bag.size());
            for (size_t i = 0; i < embedLimit; ++i)
                embs[i] = Embed(phraseFor(bag[i]));

            for (size_t i = 0; i < bag.size(); ++i)
            {
                if (i >= embedLimit || embs[i].empty())
                {
                    int cid = static_cast<int>(centroids.size());
                    centroids.push_back({});
                    clTotalFocus.push_back(bag[i].weightedFocusSecs);
                    clMembers.push_back({ i });
                    clOf[i] = cid;
                    continue;
                }
                int   bestC   = -1;
                float bestSim = CLUSTER_COS_THRESHOLD;
                for (size_t c = 0; c < centroids.size(); ++c)
                {
                    if (centroids[c].empty()) continue;
                    float s = CosineSim(embs[i], centroids[c]);
                    if (s > bestSim) { bestSim = s; bestC = static_cast<int>(c); }
                }
                if (bestC < 0)
                {
                    int cid = static_cast<int>(centroids.size());
                    centroids.push_back(embs[i]);
                    clTotalFocus.push_back(bag[i].weightedFocusSecs);
                    clMembers.push_back({ i });
                    clOf[i] = cid;
                }
                else
                {
                    std::vector<float>& c = centroids[bestC];
                    size_t n = clMembers[bestC].size();
                    for (int d = 0; d < EMBED_DIM; ++d)
                        c[d] = (c[d] * static_cast<float>(n) + embs[i][d])
                               / static_cast<float>(n + 1);
                    float norm = 0.0f;
                    for (int d = 0; d < EMBED_DIM; ++d) norm += c[d] * c[d];
                    norm = std::sqrt(norm);
                    if (norm > 1e-9f)
                        for (int d = 0; d < EMBED_DIM; ++d) c[d] /= norm;
                    clMembers[bestC].push_back(i);
                    clTotalFocus[bestC] += bag[i].weightedFocusSecs;
                    clOf[i] = bestC;
                }
            }
        }
        else
        {
            for (size_t i = 0; i < bag.size(); ++i)
            {
                centroids.push_back({});
                clTotalFocus.push_back(bag[i].weightedFocusSecs);
                clMembers.push_back({ i });
                clOf[i] = static_cast<int>(i);
            }
        }

        // ---- Sort clusters by total focus ----------------------------
        struct Cluster
        {
            size_t               origIdx;     // original index into `centroids` / `clMembers`
            double               totalFocus;  // sum of members' weightedFocusSecs
            size_t               repIdx;
            std::vector<size_t>  members;
        };
        std::vector<Cluster> clusters;
        clusters.reserve(clMembers.size());
        for (size_t c = 0; c < clMembers.size(); ++c)
        {
            Cluster cl;
            cl.origIdx    = c;
            cl.totalFocus = clTotalFocus[c];
            cl.members    = clMembers[c];
            cl.repIdx     = clMembers[c].front();
            clusters.push_back(std::move(cl));
        }
        std::sort(clusters.begin(), clusters.end(),
                  [](const Cluster& a, const Cluster& b){
                      return a.totalFocus > b.totalFocus;
                  });

        // ---- Per-cluster theme extraction ----------------------------
        auto themeFor = [&](const Cluster& cl) -> std::string {
            // freq           = total occurrences of a token across the cluster's titles
            // titlesWith     = number of distinct titles in which the token appears
            // focusWeight    = sum of weightedFocusSecs of titles in which the token
            //                  appears.  This is the primary signal: a token from
            //                  a 99%-focus title decisively outweighs a token from
            //                  a 1%-focus virtual entry, regardless of how many
            //                  virtual entries it appears in.  Uses
            //                  weightedFocusSecs (not raw seconds) so a historically
            //                  popular but currently low-focus entry still gets
            //                  some token-level weight via the EntityStore boost.
            std::unordered_map<std::string, int>          freq;
            std::unordered_map<std::string, int>          titlesWith;
            std::unordered_map<std::string, double>       focusWeight;
            std::unordered_map<std::string, std::string>  bestSurface;
            std::vector<std::string>                      orderedLowers;

            for (size_t mi : cl.members)
            {
                double memberFocus = (std::max)(0.0, bag[mi].weightedFocusSecs);
                auto toks = ExtractContentTokens(bag[mi].bestTitle);
                std::unordered_set<std::string> seenInTitle;
                for (const auto& t : toks)
                {
                    if (freq.find(t.lower) == freq.end())
                        orderedLowers.push_back(t.lower);
                    freq[t.lower] += 1;
                    if (seenInTitle.insert(t.lower).second)
                    {
                        titlesWith[t.lower] += 1;
                        focusWeight[t.lower] += memberFocus;
                    }
                    auto& s = bestSurface[t.lower];
                    if (s.empty() ||
                       (s.size() == t.surface.size() && t.surface > s))
                        s = t.surface;
                }
            }
            if (orderedLowers.empty()) return std::string();

            const size_t titleCount = cl.members.size();

            double clusterTotalFocus = 0.0;
            for (size_t mi : cl.members)
                clusterTotalFocus += (std::max)(0.0, bag[mi].weightedFocusSecs);
            if (clusterTotalFocus <= 0.0) clusterTotalFocus = 1.0; // avoid div-by-zero

            struct ScoredTok { std::string lower; double score; size_t firstIdx; };
            std::vector<ScoredTok> scored;
            scored.reserve(orderedLowers.size());

            std::vector<float> cleanCentroid;
            if (m_modelReady)
            {
                std::string joined;
                for (const auto& lc : orderedLowers)
                {
                    if (!joined.empty()) joined.push_back(' ');
                    joined += bestSurface[lc].empty() ? lc : bestSurface[lc];
                }
                cleanCentroid = Embed(joined);
            }

            // Order candidates by focus-weighted coverage first; this is
            // the dominant signal.  Title-coverage and frequency act as
            // tiebreakers when several tokens share the same focus weight
            // (typically tokens from the same dominant title).
            std::vector<std::string> candidates = orderedLowers;
            std::sort(candidates.begin(), candidates.end(),
                      [&](const std::string& a, const std::string& b){
                          if (focusWeight[a] != focusWeight[b])
                              return focusWeight[a] > focusWeight[b];
                          if (titlesWith[a] != titlesWith[b])
                              return titlesWith[a] > titlesWith[b];
                          if (freq[a] != freq[b]) return freq[a] > freq[b];
                          return a < b;
                      });
            if (candidates.size() > 12) candidates.resize(12);

            for (size_t ci = 0; ci < candidates.size(); ++ci)
            {
                const std::string& lc = candidates[ci];
                // Base score: focus-share coverage dominates.  A token
                // from a 99%-focus title gets focusCoverage ~= 0.99 and
                // wins decisively over a token from a 1%-focus title
                // (~0.01), regardless of how many low-focus titles the
                // latter appears in.  Title coverage and log-frequency
                // act as small additive tiebreakers.
                double focusCoverage =
                    static_cast<double>(focusWeight[lc])
                    / static_cast<double>(clusterTotalFocus);
                double titleCoverage =
                    static_cast<double>(titlesWith[lc])
                    / static_cast<double>((std::max)(size_t{1}, titleCount));
                double rawFreq = static_cast<double>(freq[lc]);

                double s = focusCoverage * 5.0
                         + titleCoverage * 0.5
                         + std::log1p(rawFreq) * 0.25;

                // Aggressive penalty for tokens that carry essentially
                // no focus weight (e.g. tokens that only appear in
                // virtual file-augmentation entries while the real
                // focus is elsewhere).
                if (focusCoverage < 0.05) s *= 0.2;

                if (m_modelReady && !cleanCentroid.empty())
                {
                    std::vector<float> v = Embed(bestSurface[lc].empty() ? lc : bestSurface[lc]);
                    if (!v.empty())
                    {
                        float cs = CosineSim(v, cleanCentroid);
                        if (cs < 0.0f) cs = 0.0f;
                        s *= (1.0 + static_cast<double>(cs));
                    }
                }
                size_t firstIdx = 0;
                for (size_t k = 0; k < orderedLowers.size(); ++k)
                    if (orderedLowers[k] == lc) { firstIdx = k; break; }
                scored.push_back({ lc, s, firstIdx });
            }

            std::sort(scored.begin(), scored.end(),
                      [](const ScoredTok& a, const ScoredTok& b){
                          if (a.score != b.score) return a.score > b.score;
                          return a.firstIdx < b.firstIdx;
                      });

            // How many theme tokens to emit?  Category-mode bags are
            // forced into one virtual cluster and tend to have many
            // titles, so allow up to 3 themes when the bag is large
            // enough.  Combined-"all" mode stays at 1-2 tokens.
            size_t maxPicks = (categoryMode && titleCount >= 4) ? 3u : 2u;

            std::vector<std::string> picks;
            auto surfaceOf = [&](const std::string& lc) {
                return bestSurface[lc].empty() ? lc : bestSurface[lc];
            };
            auto wouldDuplicate = [&](const std::string& candLower) {
                for (const auto& already : picks)
                {
                    std::string al = AsciiLower(already);
                    if (al.find(candLower) != std::string::npos ||
                        candLower.find(al) != std::string::npos)
                        return true;
                }
                return false;
            };

            if (!scored.empty())
                picks.push_back(surfaceOf(scored[0].lower));

            for (size_t k = 1; k < scored.size() && picks.size() < maxPicks; ++k)
            {
                if (scored[k].score < scored[0].score * 0.5) break;
                if (wouldDuplicate(scored[k].lower)) continue;
                picks.push_back(surfaceOf(scored[k].lower));
            }

            // Re-order picks by first-appearance index in the original
            // title stream so the theme reads more naturally.
            if (picks.size() >= 2)
            {
                std::vector<size_t> idx(picks.size(), 0);
                for (size_t pi = 0; pi < picks.size(); ++pi)
                {
                    std::string lo = AsciiLower(picks[pi]);
                    for (size_t k = 0; k < orderedLowers.size(); ++k)
                        if (orderedLowers[k] == lo) { idx[pi] = k; break; }
                }
                // Tiny insertion sort -- picks.size() <= 3.
                for (size_t a = 1; a < picks.size(); ++a)
                {
                    size_t b = a;
                    while (b > 0 && idx[b - 1] > idx[b])
                    {
                        std::swap(idx[b - 1], idx[b]);
                        std::swap(picks[b - 1], picks[b]);
                        --b;
                    }
                }
            }

            std::string outStr;
            for (const auto& p : picks) { if (!outStr.empty()) outStr += " "; outStr += p; }
            return TitleCase(outStr);
        };

        auto verbFor = [&](const Cluster& cl, const std::string& theme) -> std::string {
            const AppAgg& rep = bag[cl.repIdx];
            std::string repVerb    = AsciiLower(rep.verb);
            std::string lowerTheme = AsciiLower(theme);

            auto themeHas = [&](const char* kw) -> bool {
                return lowerTheme.find(kw) != std::string::npos;
            };
            auto anyTitleHas = [&](const char* kw) -> bool {
                for (size_t mi : cl.members)
                {
                    std::string lo = AsciiLower(bag[mi].bestTitle);
                    if (lo.find(kw) != std::string::npos) return true;
                }
                return false;
            };

            if (rep.isBrowser)
            {
                if (anyTitleHas("pull request") || anyTitleHas("pr #") ||
                    anyTitleHas("merge request") || anyTitleHas("commit") ||
                    anyTitleHas("diff") || anyTitleHas("review") ||
                    anyTitleHas("issue #") || anyTitleHas("/pull/") ||
                    themeHas("review") || themeHas("commit"))
                    return "Reviewing";
                if (anyTitleHas("docs") || anyTitleHas("documentation") ||
                    anyTitleHas("api reference") || anyTitleHas("tutorial") ||
                    anyTitleHas("guide") || anyTitleHas("how to") ||
                    anyTitleHas("learn") || anyTitleHas("manual"))
                    return "Reading about";
                return "Researching";
            }
            if (repVerb.find("editing") != std::string::npos ||
                repVerb.find("coding")  != std::string::npos ||
                repVerb.find("writing") != std::string::npos)
                return "Working on";
            if (repVerb.find("messaging") != std::string::npos ||
                repVerb.find("chatting")  != std::string::npos ||
                repVerb.find("meeting")   != std::string::npos ||
                repVerb.find("calling")   != std::string::npos ||
                repVerb.find("emailing")  != std::string::npos ||
                repVerb.find("discussing")!= std::string::npos)
                return "Discussing";
            if (repVerb.find("designing") != std::string::npos ||
                repVerb.find("drawing")   != std::string::npos)
                return "Designing";
            if (repVerb.find("watching") != std::string::npos ||
                repVerb.find("playing")  != std::string::npos ||
                repVerb.find("listening")!= std::string::npos)
                return "Watching";
            if (repVerb.find("reading") != std::string::npos)
                return "Reading";
            if (repVerb.find("terminal") != std::string::npos ||
                repVerb.find("shell")    != std::string::npos)
                return "Working on";
            if (rep.verb.empty()) return "Working on";
            std::string v = rep.verb;
            if (!v.empty() && v[0] >= 'a' && v[0] <= 'z') v[0] = char(v[0] - 32);
            return v;
        };

        auto clusterPhrase = [&](const Cluster& cl) -> std::string {
            std::string theme = themeFor(cl);
            std::string p;
            if (theme.empty())
            {
                if (categoryMode)
                {
                    // Category-mode fallback: theme extraction produced
                    // nothing (titles got over-filtered by the brand /
                    // stop-word / file-extension filters).
                    //
                    // Strategy: try harder before giving up to a generic
                    // verb+friendlyName phrase.
                    //
                    //   1. cleaned bestTitle with ext stripped
                    //   2. rawTitle with ext stripped (catches cases
                    //      where CleanTitle reduced everything to the
                    //      app name, but the raw form has the doc info)
                    //   3. union of all member titles (multi-member
                    //      cluster -- maybe one member has a useful
                    //      title even if the rep doesn't)
                    //   4. friendlyName as last resort
                    //
                    // A "useful" surface is one that:
                    //   - is non-empty AND
                    //   - doesn't equal the friendly name (case-insens.) AND
                    //   - has at least 4 chars OR contains a space.
                    const AppAgg& rep = bag[cl.repIdx];
                    std::string verb = rep.verb.empty() ? std::string("Using") : rep.verb;
                    if (!verb.empty() && verb[0] >= 'a' && verb[0] <= 'z')
                        verb[0] = char(verb[0] - 32);

                    auto stripExt = [](std::string s) {
                        size_t dot = s.find_last_of('.');
                        if (dot != std::string::npos && dot > 0 && dot >= s.size() - 6)
                            s.erase(dot);
                        return s;
                    };
                    auto isUseful = [&](const std::string& s) -> bool {
                        if (s.empty()) return false;
                        std::string lo = AsciiLower(s);
                        std::string fn = AsciiLower(rep.friendlyName);
                        if (lo == fn) return false;
                        if (lo.size() < 4 && s.find(' ') == std::string::npos)
                            return false;
                        return true;
                    };

                    std::string titleSurface;
                    std::string cand;

                    cand = stripExt(rep.bestTitle);
                    if (isUseful(cand)) titleSurface = cand;

                    if (titleSurface.empty())
                    {
                        cand = stripExt(rep.rawTitle);
                        if (isUseful(cand)) titleSurface = cand;
                    }

                    if (titleSurface.empty())
                    {
                        for (size_t mi : cl.members)
                        {
                            cand = stripExt(bag[mi].bestTitle);
                            if (isUseful(cand)) { titleSurface = cand; break; }
                            cand = stripExt(bag[mi].rawTitle);
                            if (isUseful(cand)) { titleSurface = cand; break; }
                        }
                    }

                    if (!titleSurface.empty())
                    {
                        p = verb + " " + titleSurface;
                    }
                    else if (rep.friendlyName == "File")
                    {
                        p = "Working on files";
                    }
                    else if (rep.friendlyName == "Browser")
                    {
                        p = "Browsing the web";
                    }
                    else
                    {
                        // Genuinely no useful title anywhere -- avoid
                        // "Drafting Word"; emit "Working in Word"
                        // (slightly less wrong as a status line).
                        p = "Working in " + rep.friendlyName;
                    }
                }
                else
                {
                    p = phraseFor(bag[cl.repIdx]);
                }
            }
            else
            {
                std::string verb = verbFor(cl, theme);
                p = verb + " " + theme;
            }

            if (cl.members.size() >= 2)
            {
                // Build a deduped friendlyName list for the "(across …)"
                // suffix.  Two filters:
                //   1. Dedup (so the Websites bag — every virtual AppAgg
                //      has friendlyName="Browser" — doesn't render as
                //      "(across Browser, Browser & Browser)").
                //   2. Skip the virtual placeholder names "File" /
                //      "Document" / "Browser" -- they're internal markers
                //      for the augmentation paths and read awkwardly in
                //      the user-visible suffix ("across PowerPoint &
                //      File" doesn't make sense; the user has a real
                //      PowerPoint deck open, not a "file").  Real app
                //      friendly names (PowerPoint, Word, VS Code, Edge,
                //      ...) flow through unchanged.
                auto isVirtualName = [](const std::string& fn) {
                    return fn == "File" || fn == "Document" || fn == "Browser";
                };
                std::vector<std::string> uniqueNames;
                uniqueNames.reserve(cl.members.size());
                for (size_t mi : cl.members)
                {
                    const std::string& fn = bag[mi].friendlyName;
                    if (isVirtualName(fn)) continue;
                    bool seen = false;
                    for (const auto& s : uniqueNames)
                        if (s == fn) { seen = true; break; }
                    if (!seen) uniqueNames.push_back(fn);
                }
                if (uniqueNames.size() >= 2)
                {
                    std::string suffix = " (across ";
                    size_t shown = (std::min)(uniqueNames.size(), size_t{3});
                    for (size_t j = 0; j < shown; ++j)
                    {
                        if (j == 0) suffix += uniqueNames[j];
                        else if (j + 1 == shown) suffix += " & " + uniqueNames[j];
                        else                     suffix += ", " + uniqueNames[j];
                    }
                    if (uniqueNames.size() > shown)
                    {
                        std::ostringstream o;
                        o << " + " << (uniqueNames.size() - shown) << " more";
                        suffix += o.str();
                    }
                    suffix += ")";
                    p += suffix;
                }
            }
            return p;
        };

        // -----------------------------------------------------------------
        //  Umbrella detection
        //
        //  Before emitting per-cluster phrases, check whether the top-K
        //  clusters share a dominant theme token.  If so, collapse them
        //  into a single descriptive line of the form
        //
        //     "Exploring <Umbrella> and its various aspects like <F1>, <F2> and <F3>"
        //
        //  rather than emitting N separate "Working on <FootballPlayers>",
        //  "Working on <FootballStrategy>", ... lines that the user has to
        //  mentally re-bundle.
        //
        //  Trigger: a single content token (after the brand / stopword
        //  filter) appears in titles of >= 50% of the top-K (max 5)
        //  clusters by focus AND those clusters together represent >= 50%
        //  of bag focus.
        //
        //  The "facets" are the most-distinctive non-umbrella theme
        //  token from each absorbed sub-cluster.
        // -----------------------------------------------------------------
        struct Umbrella
        {
            std::string                 surface;   // display form of the umbrella token
            std::string                 lower;     // lowercase key
            std::vector<std::string>    facets;    // display tokens, ordered
            std::unordered_set<size_t>  absorbed;  // indices into `clusters` covered
            double                      focus = 0.0;
        };

        // Token set per cluster (for cross-cluster coverage analysis).
        std::vector<std::unordered_set<std::string>>            clusterTokenSets(clusters.size());
        std::vector<std::unordered_map<std::string, std::string>> clusterTokenSurfaces(clusters.size());
        std::vector<std::unordered_map<std::string, int>>       clusterTokenCounts(clusters.size());
        for (size_t i = 0; i < clusters.size(); ++i)
        {
            for (size_t mi : clusters[i].members)
            {
                auto toks = ExtractContentTokens(bag[mi].bestTitle);
                for (const auto& t : toks)
                {
                    clusterTokenSets[i].insert(t.lower);
                    clusterTokenCounts[i][t.lower] += 1;
                    auto& s = clusterTokenSurfaces[i][t.lower];
                    if (s.empty() ||
                       (s.size() == t.surface.size() && t.surface > s))
                        s = t.surface;
                }
            }
        }

        auto detectUmbrella = [&]() -> Umbrella {
            Umbrella out;
            if (clusters.size() < 2) return out;

            const size_t topK = (std::min)(clusters.size(), size_t{5});

            // For each token, count how many of the top-K clusters
            // contain it AND sum the focus of those clusters.
            std::unordered_map<std::string, int>    tokenCoverage; // cluster count
            std::unordered_map<std::string, double> tokenFocus;
            std::unordered_map<std::string, std::string> tokenSurface;
            for (size_t i = 0; i < topK; ++i)
            {
                for (const auto& lc : clusterTokenSets[i])
                {
                    tokenCoverage[lc] += 1;
                    tokenFocus[lc]   += clusters[i].totalFocus;
                    auto& s = tokenSurface[lc];
                    const auto& candSurface = clusterTokenSurfaces[i][lc];
                    if (s.empty() ||
                       (s.size() == candSurface.size() && candSurface > s))
                        s = candSurface;
                }
            }

            // Pick the highest-coverage token (focus as tiebreaker).
            std::string bestLower;
            int         bestCoverage = 0;
            double      bestFocus    = 0.0;
            for (const auto& kv : tokenCoverage)
            {
                if (kv.first.size() < 4) continue;       // tiny tokens are too noisy
                if (kv.second  > bestCoverage ||
                   (kv.second == bestCoverage && tokenFocus[kv.first] > bestFocus))
                {
                    bestLower    = kv.first;
                    bestCoverage = kv.second;
                    bestFocus    = tokenFocus[kv.first];
                }
            }
            if (bestLower.empty()) return out;

            // Trigger conditions:
            //   * at least 2 absorbed clusters
            //   * >= 50% of top-K clusters contain the umbrella token
            //   * >= 50% of bag focus contributed by absorbed clusters
            if (bestCoverage < 2)              return out;
            if (bestCoverage * 2 < (int)topK)  return out;
            if (bagTotalFocus > 0.0 && bestFocus * 2.0 < bagTotalFocus) return out;

            out.surface = TitleCase(tokenSurface[bestLower].empty()
                                        ? bestLower : tokenSurface[bestLower]);
            out.lower   = bestLower;
            out.focus   = bestFocus;
            for (size_t i = 0; i < topK; ++i)
            {
                if (clusterTokenSets[i].count(bestLower))
                    out.absorbed.insert(i);
            }

            // Build the facet list.  For each absorbed sub-cluster pick
            // its most-distinctive non-umbrella token: highest local
            // frequency, but penalize tokens that are widely shared
            // across other absorbed clusters (those are co-themes, not
            // facets).  Dedupe + cap at 4.
            std::unordered_map<std::string, int> facetCrossCluster;
            for (size_t ci : out.absorbed)
                for (const auto& lc : clusterTokenSets[ci])
                    facetCrossCluster[lc] += 1;

            std::unordered_set<std::string> seenFacet;
            seenFacet.insert(bestLower);
            std::vector<std::pair<std::string, std::string>> facetsOrdered; // (display, lower)
            for (size_t ci : out.absorbed)
            {
                std::string bestFacet;
                std::string bestFacetSurface;
                double      bestScore = -1.0;
                for (const auto& kv : clusterTokenCounts[ci])
                {
                    const std::string& lc = kv.first;
                    if (lc == bestLower) continue;
                    if (lc.size() < 4)   continue;
                    if (seenFacet.count(lc)) continue;
                    if (lc.find(bestLower) != std::string::npos) continue;
                    if (bestLower.find(lc) != std::string::npos) continue;
                    double freq  = static_cast<double>(kv.second);
                    int    cross = facetCrossCluster[lc];
                    // Prefer locally frequent + locally distinctive
                    // tokens.  cross > 1 means another absorbed sub-
                    // cluster also has this token -- demote it.
                    double score = freq / (1.0 + 0.6 * (cross - 1));
                    if (score > bestScore)
                    {
                        bestScore        = score;
                        bestFacet        = lc;
                        bestFacetSurface = clusterTokenSurfaces[ci][lc];
                    }
                }
                if (bestFacet.empty()) continue;
                seenFacet.insert(bestFacet);
                std::string disp = bestFacetSurface.empty() ? bestFacet : bestFacetSurface;
                facetsOrdered.push_back({ TitleCase(disp), bestFacet });
                if (facetsOrdered.size() >= 4) break;
            }
            for (const auto& f : facetsOrdered) out.facets.push_back(f.first);
            return out;
        };

        auto formatUmbrellaPhrase = [&](const Umbrella& u) -> std::string {
            // Pick a verb that suits the rep-app type of the absorbed
            // clusters.  Browser-dominant -> "Researching"; editor-
            // dominant -> "Working on"; mixed -> "Exploring" (default).
            int browserCount = 0, editorCount = 0;
            for (size_t ci : u.absorbed)
            {
                const AppAgg& rep = bag[clusters[ci].repIdx];
                if (rep.isBrowser) browserCount++;
                else
                {
                    std::string v = AsciiLower(rep.verb);
                    if (v.find("editing")  != std::string::npos ||
                        v.find("drafting") != std::string::npos ||
                        v.find("working")  != std::string::npos)
                        editorCount++;
                }
            }
            std::string verb = "Exploring";
            if (browserCount > 0 && editorCount == 0) verb = "Researching";
            else if (editorCount > 0 && browserCount == 0) verb = "Working on";

            if (u.facets.size() >= 3)
            {
                // "Exploring X and its various aspects like A, B and C"
                size_t shown = (std::min)(u.facets.size(), size_t{4});
                std::string tail;
                for (size_t i = 0; i < shown; ++i)
                {
                    if (i == 0)                          tail += u.facets[i];
                    else if (i + 1 == shown)             tail += " and " + u.facets[i];
                    else                                 tail += ", "    + u.facets[i];
                }
                return verb + " " + u.surface +
                       " and its various aspects like " + tail;
            }
            else if (u.facets.size() == 2)
            {
                return verb + " " + u.surface +
                       " across " + u.facets[0] + " and " + u.facets[1];
            }
            else if (u.facets.size() == 1)
            {
                return verb + " " + u.surface + " (focus: " + u.facets[0] + ")";
            }
            return verb + " " + u.surface;
        };

        Umbrella umbrella = detectUmbrella();
        // Require at least 2 facets for the umbrella line to be more
        // informative than the individual sub-cluster phrases.
        bool umbrellaActive = (umbrella.facets.size() >= 2);

        // ---- Adaptive top-N --------------------------------------
        //
        // Emit at most 3 lines.  If the umbrella fired, line 1 is the
        // umbrella phrase and we skip all absorbed clusters; the
        // remaining lines come from clusters not absorbed (subject to
        // the 5% focus floor in non-category mode).
        //
        // The trailing "+ N other thread(s)" suffix is **gone** -- the
        // summary is multi-line now, so either a thread is informative
        // enough to deserve its own line or it's dropped silently.
        const size_t kMaxSummaryLines     = 3;
        const int    kMinClusterPctOfFocus = 5;
        std::vector<std::string> lines;
        double covered  = 0.0;
        size_t included = 0;

        if (umbrellaActive)
        {
            std::string ulin = formatUmbrellaPhrase(umbrella);
            if (!ulin.empty())
            {
                lines.push_back(std::move(ulin));
                included = 1;
                covered  = umbrella.focus;
            }
        }

        for (size_t i = 0; i < clusters.size(); ++i)
        {
            if (included >= kMaxSummaryLines) break;
            if (umbrellaActive && umbrella.absorbed.count(i)) continue;
            if (!categoryMode && included >= 1 && bagTotalFocus > 0.0)
            {
                int clusterPct =
                    static_cast<int>((clusters[i].totalFocus * 100.0)
                                     / bagTotalFocus);
                if (clusterPct < kMinClusterPctOfFocus) continue;
            }
            if (bagTotalFocus > 0.0
             && static_cast<int>((covered * 100.0) / bagTotalFocus) >= 90)
                break;
            std::string phrase = clusterPhrase(clusters[i]);
            if (phrase.empty()) continue;
            lines.push_back(std::move(phrase));
            covered += clusters[i].totalFocus;
            ++included;
        }

        out.lines       = std::move(lines);
        out.clusterOf   = std::move(clOf);
        out.threadCount = static_cast<int>(clusters.size());
        return out;
    };

    // ---- "All" composition (also produces the items[] thread IDs) -
    OneLinerBuild allBuild = totalFocusSecs > 0 ? composeOneLinerFromBag(ranked) : OneLinerBuild{};
    snap.threadCount       = allBuild.threadCount;
    std::vector<int>& clusterOf = allBuild.clusterOf;
    if (clusterOf.empty()) clusterOf.assign(ranked.size(), -1);
    snap.model = m_modelReady ? m_modelName : std::string("deterministic");

    // ---- Structured breakdown (per-entry top 5) -------------------
    for (size_t i = 0; i < ranked.size() && snap.items.size() < MAX_ITEMS; ++i)
    {
        ContextSnapshot::AppItem it;
        it.app          = ranked[i].friendlyName;
        it.exe          = ExeBasenameUtf8(ToLower(ranked[i].exeName));
        it.title        = ranked[i].bestTitle;
        it.rawTitle     = ranked[i].rawTitle;
        it.focusSeconds = ranked[i].totalFocusSecs;
        it.pct          = totalFocusSecs > 0
            ? static_cast<int>((ranked[i].totalFocusSecs * 100LL) / totalFocusSecs)
            : 0;
        it.threadId     = (i < clusterOf.size() && clusterOf[i] >= 0)
                              ? clusterOf[i] + 1
                              : 0;
        it.entityKey    = EntityKeyOfAgg(ranked[i]);
        snap.items.push_back(std::move(it));
    }

    // ---- Current entities (INTEL-2) --------------------------------
    // Joinable keys + roles + clipped active seconds: what a search app boosts on.
    {
        static const char* const kCommApps[] = { "Outlook", "Teams", "Slack", "Discord", "WhatsApp", "Zoom", "Skype", "Telegram", "Mail" };
        std::vector<std::string> keys;
        for (size_t i = 0; i < ranked.size() && snap.entities.size() < 12; ++i)
        {
            const AppAgg& a = ranked[i];
            ContextSnapshot::EntityItem e;
            e.entityKey = EntityKeyOfAgg(a);
            if (e.entityKey.empty()) continue;
            e.entityType = !a.entityType.empty() ? a.entityType : (!a.filePath.empty() ? "file" : (!a.webUrl.empty() ? "url" : "app"));
            e.display = !a.entityDisplay.empty() ? WideToUtf8(a.entityDisplay)
                      : (!a.filePath.empty() ? WideToUtf8(a.filePath) : (!a.webUrl.empty() ? WideToUtf8(a.webUrl) : a.friendlyName));
            e.title = a.bestTitle;
            e.app = a.friendlyName;
            bool comm = false;
            for (const char* c : kCommApps) if (a.friendlyName.find(c) != std::string::npos) { comm = true; break; }
            e.role = comm ? "communication" : (snap.entities.size() < 2 && a.totalFocusSecs >= 30 ? "focus" : "reference");
            e.activeSecs = a.totalFocusSecs;
            e.exposureSecs = a.exposureSecs;
            e.passiveSecs = a.passiveSecs;
            e.idleSecs = a.idleSecs;
            e.unknownSecs = a.unknownSecs;
            e.action = a.action.empty() ? "foreground" : a.action;
            e.resolution = a.resolution;
            if (a.totalFocusSecs == 0)
                e.role = a.unknownSecs > 0 ? "attention_unknown" : (a.passiveSecs > 0 ? "passive" : "observed");
            e.lastObservedTs = a.lastSeenTs;
            e.lastActiveTs = a.totalFocusSecs > 0 ? a.lastSeenTs : 0;
            keys.push_back(e.entityKey);
            snap.entities.push_back(std::move(e));
        }
        if (entities && !keys.empty())
        {
            std::vector<EntityScore> sc = entities->Score(keys, "long", snap.timestamp, false);
            for (size_t i = 0; i < sc.size() && i < snap.entities.size(); ++i)
                if (sc[i].found) snap.entities[i].score = sc[i].score;
        }
    }

    if (totalFocusSecs == 0)
    {
        for (const auto& a : ranked)
        {
            const std::string label = a.actionLabel.empty() ? "Foreground observed" : a.actionLabel;
            const std::string line = label + " \"" + (a.bestTitle.empty() ? a.friendlyName : a.bestTitle) + "\"";
            if (snap.summary.size() < 3) snap.summary.push_back(line);
            auto& facet = a.isBrowser || a.entityType == "url" ? snap.summaryWebsites :
                (a.entityType != "app" && (a.entityType == "file" || IsFileAppFriendlyName(a.friendlyName)) ? snap.summaryFiles : snap.summaryApps);
            if (facet.size() < 3) facet.push_back(line);
        }
        snap.confidence = ranked.empty() ? 0.0 : (snap.unknownSeconds > 0 ? 0.15 : 0.25);
        if (ranked.empty())
        {
            snap.summary.push_back("(no recent user activity)");
        }
        return snap;
    }

    // ---- Canonical "all" summary: rich deterministic listing (v5.17)
    // The cluster->theme token output (`allBuild.lines`) is kept as the
    // LLM topic hint (`themeAll`); the user-facing summary is the
    // richer, more grounded listing produced by ComposeRichListing.
    std::vector<std::string> themeAll = allBuild.lines;
    snap.summary = ComposeRichListing(ranked, "all", themeAll);

    // -----------------------------------------------------------------
    // Per-category summaries.  Each is an *independent* projection of
    // the same activity window, composed through the same pipeline
    // above.  A consumer picks one of {all, files, websites, apps} via
    // the dropdown to narrow the surface.
    // the dropdown to narrow the surface.
    //
    // Categorization rules (in priority order, first match wins):
    //   * Browsers (isBrowser=true) feed Websites only.
    //   * Apps known to be comms / media / terminals / remote / VCS UI
    //     (`IsAppOnlyFriendlyName`) feed Apps only -- even if the title
    //     mentions a filename (e.g. a Teams chat message about a PDF).
    //   * Files apps -- editors, IDEs, Office, PDF readers, note apps,
    //     image viewers / editors, design tools (`IsFileAppFriendlyName`)
    //     -- feed Files.
    //   * Generic / unrecognized apps with a window title that contains
    //     a recognized file extension (`TitleLooksLikeFileActivity`)
    //     also feed Files -- this catches arbitrary apps the user has
    //     opened a file in (e.g. `report.docx - LibreOffice Writer`,
    //     `notes.md - GenericMarkdownApp`).
    //   * Everything else feeds Apps.
    // Files / Websites / Apps therefore form a strict partition of
    // ranked; an app that is in one is never in another.
    // -----------------------------------------------------------------
    auto isFileApp = [](const AppAgg& a) -> bool {
        if (a.isBrowser) return false;
        if (IsAppOnlyFriendlyName(a.friendlyName)) return false;
        if (IsFileAppFriendlyName(a.friendlyName)) return true;
        if (TitleLooksLikeFileActivity(a.bestTitle)) return true;
        if (TitleLooksLikeFileActivity(a.rawTitle))  return true;
        return false;
    };

    std::vector<AppAgg> rankedFiles;
    std::vector<AppAgg> rankedApps;
    rankedFiles.reserve(ranked.size());
    rankedApps.reserve(ranked.size());
    for (const auto& a : ranked)
    {
        if (a.isBrowser) continue;
        if (isFileApp(a)) rankedFiles.push_back(a);
        else              rankedApps.push_back(a);
    }

    // Augment Files with recently-touched file basenames captured by
    // FileCapture.  Two design constraints:
    //
    //   (a) Strict allowlist of user-content extensions only.  The
    //       previous "deny .dll/.log/.tmp/.bak" was too loose -- it
    //       let through Outlook .ost/.nst/.pst mail stores, sticky
    //       notes .sqlite, .config / .xml / .json app data, .lnk
    //       shortcuts, etc.  Whose basenames frequently include
    //       directory or user-identity tokens (the user's email
    //       address, their AppData path components, etc.) that then
    //       polluted the Files theme.
    //
    //   (b) Skip augmentation entirely when a real file app is
    //       actively in focus.  When the user is sitting in Excel
    //       editing a spreadsheet, the file-activity pool is irrelevant
    //       background noise -- typing in Excel generates dozens of
    //       writes to OneDrive caches, Office auto-recover files,
    //       and similar transient artefacts that we *cannot* fully
    //       filter at the capture layer because some of those
    //       artefacts are real user content (e.g. Outlook .ost).
    //       Bypassing the augmentation when there's an obvious
    //       focused-file signal preserves correctness for the
    //       common case without losing the "briefly-touched file"
    //       capability for the dormant-app case.
    {
        // (b): refined "skip augmentation" gate.  The old gate skipped
        // augmentation whenever any real file app had >= 30 s of focus.
        // That backfired when Word/Excel/PowerPoint had a generic
        // title like "Word" (no document name visible in the title
        // bar -- happens with certain Office 365 modes), because the
        // focused app's title yielded ZERO content tokens AND the
        // augmentation that would have surfaced the actual filename
        // was suppressed.
        //
        // Refined gate: only skip augmentation when the focused file
        // apps already provide a usable signal -- defined as at least
        // one extractable content token across their titles.  When
        // they don't (titles like "Word", "Excel", "PowerPoint"
        // alone), let augmentation through so the actual filename
        // from FileCapture surfaces in the summary.
        bool haveFocusedFileApp        = false;
        bool focusedAppHasUsefulTitle  = false;
        for (const auto& a : rankedFiles)
        {
            if (a.totalFocusSecs < 30) continue;
            haveFocusedFileApp = true;
            auto toks = ExtractContentTokens(a.bestTitle);
            if (!toks.empty()) { focusedAppHasUsefulTitle = true; break; }
        }
        bool skipAugmentation = haveFocusedFileApp && focusedAppHasUsefulTitle;

        if (!skipAugmentation)
        {
            // (a): user-content extension allowlist.  Mirrors the
            // extension set in TitleLooksLikeFileActivity above (the
            // two should stay in sync).  An empty/missing extension
            // is rejected -- "anonymous" basenames are almost never
            // user content.
            static const std::unordered_set<std::string> kUserContentExt = {
                "doc","docx","rtf","odt",
                "xls","xlsx","csv","tsv","ods",
                "ppt","pptx","odp",
                "pdf","epub","mobi",
                "txt","md","rst","tex","one",
                "jpg","jpeg","png","gif","bmp","tif","tiff",
                "webp","heic","heif","raw","dng","cr2","nef",
                "psd","ai","eps","svg","ico",
                "mp3","wav","flac","m4a","aac",
                "mp4","mkv","mov","avi","webm",
                "c","cpp","cc","cxx","h","hpp","hh","hxx",
                "cs","java","kt","scala","groovy",
                "py","rb","pl","php","js","mjs","cjs","jsx","ts","tsx",
                "go","rs","swift","m","mm","dart",
                "sh","bash","zsh","fish","ps1","psm1","psd1",
                "html","htm","css","scss","sass","less",
                "xml","json","yaml","yml","toml","ini","cfg","conf",
                "sql","graphql","vue","svelte",
                "r","jl","lua","ex","exs","erl","hs","clj",
                "zip","rar","7z","tar","gz","tgz"
            };

            auto extensionOf = [](const std::string& name) -> std::string {
                size_t dot = name.find_last_of('.');
                if (dot == std::string::npos || dot + 1 >= name.size())
                    return std::string();
                std::string ext = name.substr(dot + 1);
                // Lowercase ASCII.
                for (char& c : ext)
                    if (c >= 'A' && c <= 'Z') c = char(c + 32);
                return ext;
            };

            std::unordered_set<std::string> seenLowered;
            for (const auto& a : rankedFiles)
                seenLowered.insert(AsciiLower(a.bestTitle));

            // Track each unique basename + its full path so the
            // EntityStore boost can look up the per-file record
            // (which is keyed by the full lowercased path).  Pick the
            // most-recently-seen full path when several map to the
            // same basename.
            struct FileVirt {
                std::string  base;
                std::wstring fullPath;
                int64_t      ts;
                int          hits;
            };
            std::unordered_map<std::string, FileVirt> byBase;
            for (const auto& f : files)
            {
                std::string p = WideToUtf8(f.path);
                if (p.empty()) continue;
                size_t slash = p.find_last_of("/\\");
                std::string base = (slash != std::string::npos)
                                       ? p.substr(slash + 1) : p;
                if (base.empty()) continue;
                std::string baseLower = AsciiLower(base);

                // Strip the Excel/Word lockfile prefix "~$" so we don't
                // treat `~$report.xlsx` as a different file from
                // `report.xlsx`; the user opened the file, not the
                // lock.  Also strips a leading "~" which is the Office
                // auto-recover prefix.
                std::string keyBase = baseLower;
                while (!keyBase.empty() && (keyBase[0] == '~' || keyBase[0] == '$'))
                    keyBase.erase(0, 1);

                std::string ext = extensionOf(keyBase);
                if (ext.empty() || kUserContentExt.count(ext) == 0)
                    continue;

                if (seenLowered.count(keyBase)) continue;

                auto it = byBase.find(keyBase);
                if (it == byBase.end())
                    byBase[keyBase] = { base, f.path, f.timestampUtc, 1 };
                else
                {
                    it->second.hits += 1;
                    if (f.timestampUtc > it->second.ts)
                    {
                        it->second.ts       = f.timestampUtc;
                        it->second.fullPath = f.path;
                    }
                }
            }
            std::vector<FileVirt> fv;
            fv.reserve(byBase.size());
            for (auto& kv : byBase) fv.push_back(std::move(kv.second));
            std::sort(fv.begin(), fv.end(),
                      [](const FileVirt& a, const FileVirt& b){ return a.ts > b.ts; });
            if (fv.size() > 6) fv.resize(6);
            for (auto& v : fv)
            {
                AppAgg agg{};
                agg.friendlyName    = "File";
                agg.verb            = "Editing";
                agg.bestTitle       = v.base;
                agg.rawTitle        = v.base;
                agg.filePath        = v.fullPath;   // for EntityStore lookup
                // Cap synthetic focus at 15 s per virt regardless of
                // file-activity hit volume.  Real foreground sessions
                // (typically 30 s -- many minutes) thus dominate the
                // theme even when several virts are present.
                int synth = (std::min)(15, (std::max)(3, v.hits * 3));
                agg.totalFocusSecs  = synth;
                agg.lastSeenTs      = v.ts;
                agg.isBrowser       = false;
                rankedFiles.push_back(std::move(agg));
            }
            // Apply the bounded EntityStore boost across the whole rankedFiles
            // bag (real focused-file entries + the file-activity virtuals just
            // added), keyed by resolved document / file path / app.
            ApplyInferenceBoost(rankedFiles, entities, [](const AppAgg& a) { return EntityKeyOfAgg(a); }, snap.timestamp);
            std::sort(rankedFiles.begin(), rankedFiles.end(),
                      [](const AppAgg& a, const AppAgg& b){
                          if (a.weightedFocusSecs != b.weightedFocusSecs)
                              return a.weightedFocusSecs > b.weightedFocusSecs;
                          return a.lastSeenTs > b.lastSeenTs;
                      });
        }
    }

    // rankedApps was split off `ranked` before the file-activity virt
    // augmentation, but ApplyInferenceBoost on `ranked` already
    // populated weightedFocusSecs on each entry (file apps and
    // non-file apps alike).  No need to re-run the boost on
    // rankedApps -- the values flowed in via the copy.  Re-sort just
    // in case (cheap; rankedApps is <=20 entries) to keep the
    // weighted ordering after the split.
    std::sort(rankedApps.begin(), rankedApps.end(),
              [](const AppAgg& a, const AppAgg& b){
                  if (a.weightedFocusSecs != b.weightedFocusSecs)
                      return a.weightedFocusSecs > b.weightedFocusSecs;
                  return a.lastSeenTs > b.lastSeenTs;
              });

    // Build the Websites bag: real dwell from browser focus sessions that resolved a page,
    // else WebCapture visit rows (dwell attributed per window, or a small per-visit proxy).
    // Cap at 16 entries so we don't blow ONNX cost on hyper-active browsing windows.
    std::vector<AppAgg> rankedWeb;
    {
        // Real dwell first: browser focus sessions that resolved a URL entity (INTEL-1).
        std::unordered_map<std::string, AppAgg> webByTitle;   // key: entity key, else lowercased title
        std::unordered_set<std::string> withRealDwell;
        for (const auto& a : ranked)
        {
            if (!a.isBrowser || a.entityKey.empty() || a.totalFocusSecs <= 0) continue;
            AppAgg w = a;
            w.friendlyName = "Browser";
            w.verb = "Reading";
            webByTitle[a.entityKey] = w;
            withRealDwell.insert(a.entityKey);
        }
        // Visits without a focus session. WebCapture closes a page when the next one appears in the
        // same window, so a page's on-screen time travels on the NEXT row of that window
        // (dwellSecs). The page still open in a window, and rows from unknown windows, get a
        // small per-visit proxy.
        std::vector<const BrowsingActivity*> ordered;
        ordered.reserve(browse.size());
        for (const auto& b : browse) ordered.push_back(&b);
        std::sort(ordered.begin(), ordered.end(), [](const BrowsingActivity* x, const BrowsingActivity* y) {
            if (x->windowId != y->windowId) return x->windowId < y->windowId;
            if (x->timestampUtc != y->timestampUtc) return x->timestampUtc < y->timestampUtc;
            return x->id < y->id;
        });
        for (size_t i = 0; i < ordered.size(); ++i)
        {
            const BrowsingActivity& b = *ordered[i];
            std::string raw = WideToUtf8(b.title);
            if (raw.empty()) continue;
            std::string clean = CleanTitle(raw, "Browser");
            if (clean.empty()) continue;
            std::string key = !b.entityKey.empty() ? b.entityKey : AsciiLower(clean);
            auto& agg = webByTitle[key];
            if (agg.bestTitle.empty())
            {
                agg.bestTitle    = clean;
                agg.rawTitle     = raw;
                agg.friendlyName = "Browser";
                agg.verb         = "Reading";
                agg.isBrowser    = true;
                if (!b.entityKey.empty()) { agg.entityKey = b.entityKey; agg.entityType = "url"; }
            }
            if (b.timestampUtc > agg.lastSeenTs)
            {
                agg.lastSeenTs = b.timestampUtc;
                if (!b.url.empty()) { agg.webUrl = b.url; agg.entityDisplay = b.url; }
            }
            if (withRealDwell.count(key)) continue;
            int shown = 0;
            if (b.windowId != 0 && i + 1 < ordered.size() && ordered[i + 1]->windowId == b.windowId)
                shown = ordered[i + 1]->dwellSecs;
            agg.totalFocusSecs += shown > 0 ? (std::min)(shown, 300) : 30;
        }
        rankedWeb.reserve(webByTitle.size());
        for (auto& kv : webByTitle) rankedWeb.push_back(std::move(kv.second));

        ApplyInferenceBoost(rankedWeb, entities, [](const AppAgg& a) { return EntityKeyOfAgg(a); }, snap.timestamp);

        std::sort(rankedWeb.begin(), rankedWeb.end(),
                  [](const AppAgg& a, const AppAgg& b){
                      if (a.weightedFocusSecs != b.weightedFocusSecs)
                          return a.weightedFocusSecs > b.weightedFocusSecs;
                      return a.lastSeenTs > b.lastSeenTs;
                  });
        if (rankedWeb.size() > 16) rankedWeb.resize(16);
    }

    // ---- Canonical per-facet summaries: rich deterministic listing --
    // Themes are only a fallback / LLM hint now: compute a facet's theme only when the
    // listing yields nothing for a non-empty bag (INTEL-6: no embedding work thrown away).
    auto facetListing = [&](const std::vector<AppAgg>& bag, const char* cat) -> std::vector<std::string> {
        std::vector<std::string> lst = ComposeRichListing(bag, cat, std::vector<std::string>());
        if (lst.empty() && !bag.empty())
            lst = ComposeRichListing(bag, cat, composeOneLinerFromBag(bag, /*categoryMode=*/true).lines);
        return lst;
    };
    snap.summaryFiles    = facetListing(rankedFiles, "files");
    snap.summaryWebsites = facetListing(rankedWeb,   "websites");
    snap.summaryApps     = facetListing(rankedApps,  "apps");

    // ---- LLM refinement (opt-in, gated) -----------------------------
    // The rich deterministic listing is canonical (v5.17 A/B: the 0.6B model matched it at
    // best, at ~4.6 s of CPU per call). The LLM therefore runs only when ALL of these hold:
    //   * enabled in Settings (LlmEnabled, off by default) and loaded,
    //   * a periodic compose (never on a client's request path, INTEL-7),
    //   * on AC power unless LlmAllowedOnBattery, and while the user is at the machine (PERF-1),
    //   * the combined "all" summary only, and only when its inputs changed,
    // and its output replaces the listing only when the groundedness gate can actually be
    // evaluated (granite loaded, all three embeddings present) and passes (INTEL-5).
    WarpSettings cfg = Settings::Instance().Get();
    bool onBattery = false;
    {
        SYSTEM_POWER_STATUS ps = {};
        if (GetSystemPowerStatus(&ps)) onBattery = ps.ACLineStatus == 0;
    }
    const bool userPresent = snap.presence.empty() || snap.presence == "active" || snap.presence == "passive";
    const bool llmLoaded = llm && llm->IsAvailable();
    snap.modelPolish = llmLoaded ? llm->ModelName() : std::string("(not loaded)");
    const bool llmWillRun = llmLoaded && allowLlm && cfg.llmEnabled && userPresent &&
                            (!onBattery || cfg.llmAllowedOnBattery) && m_modelReady;
    if (llmWillRun)
    {
        std::vector<LlmActivityItem> llmItemsAll;
        llmItemsAll.reserve(snap.items.size());
        std::string inputSig;
        for (const auto& it : snap.items)
        {
            LlmActivityItem li;
            li.app          = it.app;
            li.title        = it.title;
            li.rawTitle     = it.rawTitle;
            li.focusSeconds = it.focusSeconds;
            li.pct          = it.pct;
            inputSig += it.app + "\x1f" + it.title + "\x1e";
            llmItemsAll.push_back(std::move(li));
        }

        // Re-use the previous refinement when the inputs are unchanged.
        static std::mutex s_llmMtx;
        static std::string s_lastSig;
        static std::vector<std::string> s_lastListing, s_lastOutput;
        bool reused = false;
        {
            std::lock_guard<std::mutex> lk(s_llmMtx);
            if (!s_lastSig.empty() && s_lastSig == inputSig && s_lastListing == snap.summary)
            {
                if (!s_lastOutput.empty()) snap.summary = s_lastOutput;
                reused = true;
            }
        }

        if (!reused && !llmItemsAll.empty())
        {
            auto joinLines = [](const std::vector<std::string>& v) -> std::string {
                std::string s;
                for (size_t i = 0; i < v.size(); ++i) { if (i) s += ". "; s += v[i]; }
                return s;
            };
            std::vector<std::string> listing = snap.summary;
            std::vector<std::string> accepted;
            std::vector<std::string> generated = llm->Polish(themeAll, llmItemsAll, "all");
            if (!generated.empty())
            {
                std::string titles;
                for (const auto& it : llmItemsAll)
                {
                    if (!titles.empty()) titles += ". ";
                    titles += it.title.empty() ? it.rawTitle : it.title;
                }
                std::vector<float> eT = Embed(titles);
                std::vector<float> eG = Embed(joinLines(generated));
                std::vector<float> eC = Embed(joinLines(listing));
                // INTEL-5: no score => no override (the old code fell through to `canonical = generated`).
                if (!eT.empty() && !eG.empty() && !eC.empty() && CosineSim(eG, eT) + 0.02f >= CosineSim(eC, eT))
                    accepted = generated;
            }
            if (!accepted.empty()) snap.summary = accepted;
            std::lock_guard<std::mutex> lk(s_llmMtx);
            s_lastSig = inputSig;
            s_lastListing = listing;
            s_lastOutput = accepted;
        }
    }

    // ---- Granite translator: embed the canonical summary -----------
    // After the brain has produced the final user-facing summary
    // (LLM-generated when loaded, algorithmic when not), use the
    // granite embedding model as the "translator" to produce a
    // 384-dim L2-normalized vector representation of the combined
    // summary, suitable for similarity search / clustering across
    // snapshots in downstream consumers.  Empty when the granite
    // model isn't loaded or the summary is empty.
    if (m_modelReady && !snap.summary.empty())
    {
        std::string joined;
        for (size_t i = 0; i < snap.summary.size(); ++i)
        {
            if (i) joined += ". ";
            joined += snap.summary[i];
        }
        snap.summaryEmbedding = Embed(joined);
    }

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
std::string ContextInference::SnapshotToJsonObject(const ContextSnapshot& s,
                                                   const std::string& category,
                                                   bool includeVector)
{
    // category == "all": combined summary as `summary` plus the three per-category summaries.
    // category == "files" / "websites" / "apps": only that category's summary, as `summary`.
    const std::vector<std::string>* topSummary = &s.summary;
    if (category == "files")         topSummary = &s.summaryFiles;
    else if (category == "websites") topSummary = &s.summaryWebsites;
    else if (category == "apps")     topSummary = &s.summaryApps;

    // One compliant writer (API-7): every control character escaped, invalid UTF-8 (e.g. a
    // title cut mid-sequence, INTEL-8) replaced by U+FFFD, NaN written as null.
    std::string out;
    WarpJson::Writer w(out);
    auto lines = [&w](const char* key, const std::vector<std::string>& v) {
        w.Key(key).BeginArray();
        for (const std::string& l : v) w.String(l);
        w.EndArray();
    };
    w.BeginObject()
     .KV("timestamp", s.timestamp)
     .KV("window_start", s.windowStartTs)
     .KV("window_end", s.windowEndTs)
     .KV("window_seconds", s.windowSeconds)
     .KV("category", category);
    lines("summary", *topSummary);
    if (category == "all")
    {
        lines("summary_files", s.summaryFiles);
        lines("summary_websites", s.summaryWebsites);
        lines("summary_apps", s.summaryApps);
    }
    w.KV("activity_count", (int64_t)s.activityCount)
     .KV("focus_seconds", (int64_t)s.focusSeconds)
     .KV("foreground_exposure_seconds", (int64_t)s.exposureSeconds)
     .KV("passive_seconds", (int64_t)s.passiveSeconds)
     .KV("idle_seconds", (int64_t)s.idleSeconds)
     .KV("unknown_attention_seconds", (int64_t)s.unknownSeconds)
     .KV("attention_semantics", "interaction_active_v1")
     .KV("dominant_focus_pct", (int64_t)s.dominantPct)
     .KV("confidence", s.confidence)
     .KV("thread_count", (int64_t)s.threadCount)
     .KV("thread_id", s.threadId)
     .KV("presence", s.presence)
     .KV("model", s.model)
     .KV("model_polish", s.modelPolish);
    lines("signal_types", s.signalTypes);
    if (includeVector)
    {
        w.Key("summary_embedding").BeginArray();
        for (float f : s.summaryEmbedding) w.Double(std::round((double)f * 1e6) / 1e6);
        w.EndArray();
    }
    w.Key("items").BeginArray();
    for (const auto& it : s.items)
    {
        w.BeginObject().KV("app", it.app).KV("exe", it.exe).KV("title", it.title).KV("raw_title", it.rawTitle)
         .KV("focus_seconds", (int64_t)it.focusSeconds).KV("pct", (int64_t)it.pct).KV("thread_id", (int64_t)it.threadId);
        if (!it.entityKey.empty()) w.KV("entity_key", it.entityKey);
        w.EndObject();
    }
    w.EndArray();
    w.Key("current_entities").BeginArray();
    for (const auto& e : s.entities)
    {
        w.BeginObject().KV("entity_key", e.entityKey).KV("entity_type", e.entityType).KV("display", e.display)
         .KV("title", e.title).KV("app", e.app).KV("role", e.role).KV("active_secs_in_window", (int64_t)e.activeSecs)
         .KV("foreground_exposure_secs", e.exposureSecs).KV("passive_secs", e.passiveSecs)
         .KV("idle_secs", e.idleSecs).KV("unknown_attention_secs", e.unknownSecs)
         .KV("observed_action", e.action).KV("resolution", e.resolution)
         .KV("last_active_ts", e.lastActiveTs).KV("last_observed_ts", e.lastObservedTs)
         .KV("score", e.score).KV("thread_id", s.threadId).EndObject();
    }
    w.EndArray();
    w.EndObject();
    return out;
}

// Normalize the requested category to one of the four known values.
// Empty / unknown / "all" all map to "all".  The legacy value
// "documents" is accepted as a backward-compat alias for "files".
static std::string NormalizeCategory(const std::string& c)
{
    if (c == "files" || c == "websites" || c == "apps") return c;
    if (c == "documents") return "files";    // backward compatibility
    return "all";
}

// Acceptable window lengths (seconds).  The UI dropdown surfaces
// exactly these values; any request outside this set is clamped to
// the nearest valid value, defaulting to 15 min for empty / invalid.
static const int64_t kAllowedWindowSecs[] = {
    300,        //   5 min
    900,        //  15 min  (default)
    1800,       //  30 min
    3600,       //   1 h
    7200,       //   2 h
    21600,      //   6 h
    86400,      //  24 h
    604800,     //   7 d
    1296000,    //  15 d
    2592000     //  30 d
};

static int64_t NormalizeWindowSecs(int64_t requested)
{
    if (requested <= 0) return 900;
    // Snap to the nearest allowed value (ratio-based -- closer to log
    // scale than linear so we don't bias toward the bigger windows).
    int64_t best = 900;
    double  bestRatio = 1e18;
    for (int64_t v : kAllowedWindowSecs)
    {
        double r = (requested > v)
            ? static_cast<double>(requested) / static_cast<double>(v)
            : static_cast<double>(v) / static_cast<double>(requested);
        if (r < bestRatio) { bestRatio = r; best = v; }
    }
    return best;
}

std::string ContextInference::GetRecentContext(const std::string& category,
                                               int64_t            windowSecs,
                                               bool               includeVector)
{
    std::string cat = NormalizeCategory(category);
    int64_t     win = NormalizeWindowSecs(windowSecs);

    // Default window: the cached snapshot from the periodic compose. Other windows are
    // composed on demand WITHOUT the LLM, cached with a TTL proportional to the window and
    // serialized so identical concurrent requests coalesce (INTEL-7).
    ContextSnapshot fresh;
    bool            useFresh = false;
    if (win != 900 && m_store && m_store->IsOpen())
    {
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const int64_t ttl = win <= 7200 ? 60 : (win <= 86400 ? 300 : 900);
        const uint64_t epoch = m_epoch.load();
        std::lock_guard<std::mutex> ck(m_composeMtx);
        auto it = m_windowCache.find(win);
        if (it != m_windowCache.end() && now - it->second.timestamp < ttl)
            fresh = it->second;
        else
        {
            fresh = ComposeSnapshot(win, /*allowLlm=*/false);
            fresh.threadId = m_historyEngine ? m_historyEngine->CurrentThreadId() : 0;
            if (m_epoch.load() == epoch) m_windowCache[win] = fresh;
        }
        useFresh = true;
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    std::string out;
    WarpJson::Writer w(out);
    w.BeginObject().Key("recent_context");
    if (useFresh)              w.Raw(SnapshotToJsonObject(fresh, cat, includeVector));
    else if (m_haveLatest)     w.Raw(SnapshotToJsonObject(m_latest, cat, includeVector));
    else                       w.Null();
    w.KV("category", cat).KV("window_seconds", win).KV("history_count", (int64_t)m_history.size()).EndObject();
    return out;
}

std::string ContextInference::GetRecentContexts(int                count,
                                                const std::string& category,
                                                int64_t            windowSecs,
                                                bool               includeVector)
{
    if (count <= 0)  count = 10;
    if (count > 200) count = 200;
    std::string cat = NormalizeCategory(category);
    int64_t     win = NormalizeWindowSecs(windowSecs);

    // Only snapshots whose timestamp falls inside the requested lookback window.
    int64_t now    = static_cast<int64_t>(std::time(nullptr));
    int64_t cutoff = now - win;

    std::vector<std::string> rows;
    int64_t oldestInMemory = INT64_MAX;
    size_t historyCount = 0;
    HistoryEngine* hist = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        historyCount = m_history.size();
        hist = m_historyEngine;
        for (auto it = m_history.rbegin(); it != m_history.rend() && (int)rows.size() < count; ++it)
        {
            if (it->timestamp < cutoff) break;
            rows.push_back(SnapshotToJsonObject(*it, cat, includeVector));
            oldestInMemory = it->timestamp;
        }
    }
    // Project the same persisted snapshot for every facet, just as the in-memory path does.
    int persisted = 0;
    if ((int)rows.size() < count && hist)
    {
        for (const std::string& j : hist->ContextSnapshotsJson(count, cutoff))
        {
            if ((int)rows.size() >= count) break;
            WarpJson::ParseResult pr = WarpJson::Parse(j);
            if (!pr.ok || !pr.value.isObject()) continue;
            if (pr.value.get("timestamp").asInt64(0) >= oldestInMemory) continue;   // already served from memory
            std::string projected;
            WarpJson::Writer pw(projected);
            pw.BeginObject();
            for (const auto& member : pr.value.members())
            {
                if (member.first == "category" || member.first == "summary" || member.first == "summary_embedding") continue;
                if (cat != "all" && (member.first == "summary_files" || member.first == "summary_websites" || member.first == "summary_apps")) continue;
                pw.Key(member.first).Value(member.second);
            }
            pw.KV("category", cat).Key("summary");
            const auto& summary = pr.value.get(cat == "all" ? "summary" : "summary_" + cat);
            if (summary.isArray()) pw.Value(summary); else pw.BeginArray().EndArray();
            if (includeVector)
            {
                pw.Key("summary_embedding");
                const auto& vector = pr.value.get("summary_embedding");
                if (vector.isArray()) pw.Value(vector); else pw.BeginArray().EndArray();
            }
            pw.EndObject();
            rows.push_back(std::move(projected));
            ++persisted;
        }
    }

    std::string out;
    WarpJson::Writer w(out);
    w.BeginObject().Key("recent_contexts").BeginArray();
    for (const std::string& r : rows) w.Raw(r);
    w.EndArray().KV("category", cat).KV("window_seconds", win).KV("returned", (int64_t)rows.size())
     .KV("from_persisted", (int64_t)persisted).KV("history_count", (int64_t)historyCount).KV("requested", (int64_t)count).EndObject();
    return out;
}

std::string ContextInference::GetCurrentContextJson(bool includeVector)
{
    FocusTracker::ActiveSession active;
    bool haveActive = m_fg && m_fg->GetCurrentSession(active);
    std::string presence;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_presence) presence = m_presence();
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    std::string out;
    WarpJson::Writer w(out);
    w.BeginObject().KV("now", static_cast<int64_t>(std::time(nullptr))).KV("presence", presence);
    w.Key("foreground");
    if (haveActive)
    {
        w.BeginObject().KV("exe_name", active.exeName).KV("exe_path", active.exePath).KV("window_title", active.windowTitle)
         .KV("since", active.startedAtUtcSecs).KV("active_secs", (int64_t)active.activeSecsSoFar)
         .KV("passive_secs", active.passiveSecsSoFar).KV("idle_secs", active.idleSecsSoFar)
         .KV("unknown_attention_secs", active.unknownSecsSoFar).KV("aumid", active.aumid)
         .KV("resolution", active.docResolution);
        if (!active.docKey.empty())
            w.KV("entity_key", active.docKey).KV("entity_type", active.docType == "file" ? "file" : "url").KV("display", active.docDisplay);
        w.EndObject();
    }
    else w.Null();
    w.Key("snapshot");
    if (m_haveLatest) w.Raw(SnapshotToJsonObject(m_latest, "all", includeVector));
    else w.Null();
    w.EndObject();
    return out;
}

void ContextInference::ClearHistory()
{
    ++m_epoch;                                   // in-flight composes are discarded (PRIV-4)
    {
        std::lock_guard<std::mutex> ck(m_composeMtx);
        m_windowCache.clear();
    }
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
    return WarpJson::WideToUtf8(w);
}
