#include "framework.h"
#include "ContextInference.h"
#include "ActivityDatabase.h"
#include "ForegroundMonitor.h"

#include <onnxruntime_cxx_api.h>

#include <ctime>
#include <cwctype>
#include <cmath>
#include <algorithm>
#include <array>
#include <numeric>
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

// True if this app's friendly name represents a *document app* — an
// editor / viewer / authoring tool whose foreground titles are
// dominated by document, file, or canvas names.  This is the canonical
// source of truth for the "Documents" facet of the per-category
// one-liner.  Browsers are intentionally excluded (they feed the
// "Websites" facet).  Comms / chat / meeting / terminal / streaming /
// remote-desktop / VM tools all feed the "Apps" facet.
//
// Anything not in this set (including unrecognized apps that get the
// generic "Working in" fallback verb) goes to Apps.  That keeps the
// fallback from leaking unknown apps (WhatsApp UWP variants, MAUI
// chat apps, random user-installed utilities) into the Documents
// facet just because they happen to share a verb substring.
static bool IsDocumentFriendlyName(const std::string& friendlyName)
{
    static const std::unordered_set<std::string> kDocApps = {
        // Code editors / IDEs
        "visual studio", "vs code", "notepad++", "sublime text",
        "atom", "intellij idea", "pycharm", "clion", "webstorm",
        "goland", "rubymine", "phpstorm", "rider", "datagrip",
        "android studio", "eclipse", "rstudio", "matlab",
        "notepad", "wordpad",
        // Office authoring (not Outlook — that's comms)
        "word", "excel", "powerpoint", "onenote", "access",
        "visio", "publisher",
        // PDF / document viewers
        "acrobat reader", "acrobat", "sumatra pdf", "foxit reader",
        // Notes / knowledge bases
        "notion", "obsidian", "evernote", "logseq",
        // Design / authoring
        "photoshop", "illustrator", "figma", "blender",
        "premiere pro", "after effects",
        // Virtual "Document" entries injected from FileMonitor
        "document",
    };
    std::string lower = Utf8Lower(friendlyName);
    return kDocApps.find(lower) != kDocApps.end();
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

// =====================================================================
//  Semantic theme extraction.  Given the cleaned per-app titles in a
//  cluster, distill 1-2 *content* tokens that describe what the user is
//  doing -- not the verbatim title text.  No fixed taxonomy: the theme
//  emerges from the actual titles.  The sentence-encoder (BGE-small or
//  the legacy MiniLM, see Init) is used (when available) to
//  weight tokens by semantic centrality to the cluster.
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
        "signup","sign","register","account","profile","user","admin"
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
//  Prefers BAAI/bge-small-en-v1.5 (`bge-small.onnx`); falls back to
//  the legacy all-MiniLM-L6-v2 (`minilm.onnx`) if BGE isn't present.
//  This is best-effort:
//  if any step fails the engine still runs in deterministic-only mode.
// =====================================================================
bool ContextInference::Init(const std::wstring& modelsDir)
{
    if (modelsDir.empty()) return true;  // explicitly skip model load

    // Tokenizer vocab
    std::string vocabPath = WideToUtf8(modelsDir) + "\\vocab.txt";
    if (!m_tokenizer.Load(vocabPath))
        return true;  // graceful degrade

    std::wstring modelPath  = modelsDir + L"\\bge-small.onnx";
    std::string  modelLabel = "bge-small-en-v1.5";

    // Backward compat: if the new BGE model file isn't present, fall
    // back to the legacy MiniLM model (same 384 dim, same tokenizer,
    // same call surface).  Lets existing installations keep working
    // through an in-place binary upgrade until the user re-downloads.
    {
        DWORD attrs = GetFileAttributesW(modelPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            std::wstring legacy = modelsDir + L"\\minilm.onnx";
            if (GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                modelPath  = std::move(legacy);
                modelLabel = "all-MiniLM-L6-v2";
            }
        }
    }

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
    m_modelName  = std::move(modelLabel);
    return true;
}

// =====================================================================
//  Sentence embedding (384-dim, mean-pool + L2-normalize).  Works with
//  either BGE-small-en-v1.5 or all-MiniLM-L6-v2 -- both export the same
//  tensor shape and tokenization.
//  Empty vector if the model isn't loaded.
// =====================================================================
std::vector<float> ContextInference::Embed(const std::string& text)
{
    if (!m_modelReady || !m_ortSession) return {};

    std::vector<int64_t> inputIds = m_tokenizer.Encode(text, MAX_SEQ_LEN);
    std::vector<int64_t> attentionMask(inputIds.size(), 1);
    std::vector<int64_t> tokenTypeIds(inputIds.size(), 0);

    while ((int64_t)inputIds.size() < MAX_SEQ_LEN)
    {
        inputIds.push_back(m_tokenizer.PadId());
        attentionMask.push_back(0);
        tokenTypeIds.push_back(0);
    }

    std::array<int64_t, 2> shape = { 1, MAX_SEQ_LEN };

    Ort::Value inputIdsTensor = Ort::Value::CreateTensor<int64_t>(
        *m_ortMemInfo, inputIds.data(), inputIds.size(),
        shape.data(), shape.size());
    Ort::Value attMaskTensor = Ort::Value::CreateTensor<int64_t>(
        *m_ortMemInfo, attentionMask.data(), attentionMask.size(),
        shape.data(), shape.size());
    Ort::Value tokTypeTensor = Ort::Value::CreateTensor<int64_t>(
        *m_ortMemInfo, tokenTypeIds.data(), tokenTypeIds.size(),
        shape.data(), shape.size());

    const char* inputNames[]  = { "input_ids", "attention_mask", "token_type_ids" };
    const char* outputNames[] = { "last_hidden_state" };

    std::vector<Ort::Value> inputTensors;
    inputTensors.push_back(std::move(inputIdsTensor));
    inputTensors.push_back(std::move(attMaskTensor));
    inputTensors.push_back(std::move(tokTypeTensor));

    std::vector<Ort::Value> outputTensors;
    try
    {
        outputTensors = m_ortSession->Run(
            Ort::RunOptions{ nullptr },
            inputNames, inputTensors.data(), 3,
            outputNames, 1);
    }
    catch (const Ort::Exception&)
    {
        return {};
    }

    const float* rawOutput = outputTensors[0].GetTensorData<float>();

    std::vector<float> embedding(EMBED_DIM, 0.0f);
    int realTokens = 0;
    for (int t = 0; t < MAX_SEQ_LEN; ++t)
    {
        if (attentionMask[t] == 0) continue;
        ++realTokens;
        for (int d = 0; d < EMBED_DIM; ++d)
            embedding[d] += rawOutput[t * EMBED_DIM + d];
    }
    if (realTokens > 0)
    {
        for (int d = 0; d < EMBED_DIM; ++d)
            embedding[d] /= static_cast<float>(realTokens);
    }

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

    // Append on material change: different one-liner (combined or any of
    // the per-category lines) OR different dominant app.
    if (snap.oneLiner          != prev.oneLiner)          return true;
    if (snap.oneLinerDocuments != prev.oneLinerDocuments) return true;
    if (snap.oneLinerWebsites  != prev.oneLinerWebsites)  return true;
    if (snap.oneLinerApps      != prev.oneLinerApps)      return true;
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
        std::string      text;
        std::vector<int> clusterOf;     // per-bag-entry cluster id, 0-based
        int              threadCount = 0;
    };

    auto composeOneLinerFromBag = [&](const std::vector<AppAgg>& bag,
                                       bool categoryMode = false) -> OneLinerBuild {
        OneLinerBuild out;
        if (bag.empty()) return out;

        // ---- Cluster the bag -----------------------------------------
        std::vector<int>                 clOf(bag.size(), -1);
        std::vector<std::vector<float>>  centroids;
        std::vector<int>                 clTotalFocus;
        std::vector<std::vector<size_t>> clMembers;
        int                              bagTotalFocus = 0;
        for (const auto& a : bag) bagTotalFocus += (std::max)(0, a.totalFocusSecs);

        if (categoryMode)
        {
            // Per-category bags (Documents / Websites / Apps) tend to be
            // small (2-16 entries) and topic-diverse.  Sentence-encoder clustering
            // on such a bag produces mostly singleton clusters whose
            // themes degrade to brand-name-only token sets — yielding
            // verbatim-title soup once the per-cluster fallback to
            // `phraseFor` kicks in.  Force every entry into ONE virtual
            // cluster instead: this pools content tokens across all
            // titles so `themeFor` has enough material to distill 1-2
            // genuinely semantic tokens, and gives `clusterPhrase` a
            // multi-member cluster (so it adds the "across X & Y"
            // suffix instead of falling back to verbatim).
            centroids.push_back({});
            clTotalFocus.push_back(bagTotalFocus);
            std::vector<size_t> members;
            members.reserve(bag.size());
            for (size_t i = 0; i < bag.size(); ++i)
            {
                members.push_back(i);
                clOf[i] = 0;
            }
            clMembers.push_back(std::move(members));
        }
        else if (m_modelReady && bag.size() >= 2)
        {
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
                    clTotalFocus.push_back(bag[i].totalFocusSecs);
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
                    clTotalFocus.push_back(bag[i].totalFocusSecs);
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
                    clTotalFocus[bestC] += bag[i].totalFocusSecs;
                    clOf[i] = bestC;
                }
            }
        }
        else
        {
            for (size_t i = 0; i < bag.size(); ++i)
            {
                centroids.push_back({});
                clTotalFocus.push_back(bag[i].totalFocusSecs);
                clMembers.push_back({ i });
                clOf[i] = static_cast<int>(i);
            }
        }

        // ---- Sort clusters by total focus ----------------------------
        struct Cluster
        {
            int                  totalFocus;
            size_t               repIdx;
            std::vector<size_t>  members;
        };
        std::vector<Cluster> clusters;
        clusters.reserve(clMembers.size());
        for (size_t c = 0; c < clMembers.size(); ++c)
        {
            Cluster cl;
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
            std::unordered_map<std::string, int>          freq;
            std::unordered_map<std::string, std::string>  bestSurface;
            std::vector<std::string>                      orderedLowers;

            for (size_t mi : cl.members)
            {
                auto toks = ExtractContentTokens(bag[mi].bestTitle);
                for (const auto& t : toks)
                {
                    if (freq.find(t.lower) == freq.end())
                        orderedLowers.push_back(t.lower);
                    freq[t.lower] += 1;
                    auto& s = bestSurface[t.lower];
                    if (s.empty() ||
                       (s.size() == t.surface.size() && t.surface > s))
                        s = t.surface;
                }
            }
            if (orderedLowers.empty()) return std::string();

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

            std::vector<std::string> candidates = orderedLowers;
            std::sort(candidates.begin(), candidates.end(),
                      [&](const std::string& a, const std::string& b){
                          if (freq[a] != freq[b]) return freq[a] > freq[b];
                          return a < b;
                      });
            if (candidates.size() > 8) candidates.resize(8);

            for (size_t ci = 0; ci < candidates.size(); ++ci)
            {
                const std::string& lc = candidates[ci];
                double s = static_cast<double>(freq[lc]);
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

            std::vector<std::string> picks;
            picks.push_back(bestSurface[scored[0].lower].empty()
                               ? scored[0].lower
                               : bestSurface[scored[0].lower]);
            if (scored.size() >= 2 && scored[1].score >= scored[0].score * 0.6)
            {
                const std::string& w1 = scored[0].lower;
                const std::string& w2 = scored[1].lower;
                if (w1.find(w2) == std::string::npos &&
                    w2.find(w1) == std::string::npos)
                {
                    picks.push_back(bestSurface[scored[1].lower].empty()
                                       ? scored[1].lower
                                       : bestSurface[scored[1].lower]);
                }
            }

            if (picks.size() == 2)
            {
                size_t p0 = 0, p1 = 0;
                for (size_t k = 0; k < orderedLowers.size(); ++k)
                {
                    if (orderedLowers[k] == scored[0].lower) p0 = k;
                    if (orderedLowers[k] == scored[1].lower) p1 = k;
                }
                if (p1 < p0) std::swap(picks[0], picks[1]);
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
                    // Category-mode fallback: NEVER quote a verbatim
                    // title (that's how the old "verbatim mish-mash"
                    // regression happened).  Use the dominant verb of
                    // the representative entry plus the friendlyName
                    // (or, for the virtual "Document" / "Browser"
                    // friendly names, a more natural phrase).
                    const AppAgg& rep = bag[cl.repIdx];
                    std::string verb = rep.verb.empty() ? std::string("Using") : rep.verb;
                    if (!verb.empty() && verb[0] >= 'a' && verb[0] <= 'z')
                        verb[0] = char(verb[0] - 32);
                    if (rep.friendlyName == "Document")
                        p = "Working on documents";
                    else if (rep.friendlyName == "Browser")
                        p = "Browsing the web";
                    else
                        p = verb + " " + rep.friendlyName;
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
                // suffix.  Without dedup the Websites bag — where every
                // virtual AppAgg has friendlyName="Browser" — would
                // render as "(across Browser, Browser & Browser)" and
                // the Documents bag (where file-virt entries also share
                // friendlyName="Document") would do the same.
                std::vector<std::string> uniqueNames;
                uniqueNames.reserve(cl.members.size());
                for (size_t mi : cl.members)
                {
                    const std::string& fn = bag[mi].friendlyName;
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

        // ---- Adaptive top-N --------------------------------------
        std::string oneLiner;
        int    covered  = 0;
        size_t included = 0;
        for (size_t i = 0; i < clusters.size(); ++i)
        {
            std::string phrase = clusterPhrase(clusters[i]);
            std::string candidate = oneLiner.empty()
                ? phrase
                : oneLiner + " \xC2\xB7 " + phrase;
            if (i > 0
             && (candidate.size() > ONE_LINER_BUDGET
              || (bagTotalFocus > 0
                  && static_cast<int>((covered * 100LL) / bagTotalFocus) >= 80)))
                break;
            oneLiner = candidate;
            covered += clusters[i].totalFocus;
            ++included;
        }
        if (included < clusters.size())
        {
            size_t remaining = clusters.size() - included;
            std::ostringstream o;
            const char* label = (remaining == 1) ? " other thread" : " other threads";
            o << oneLiner << " \xC2\xB7 + " << remaining << label;
            oneLiner = o.str();
        }

        out.text        = std::move(oneLiner);
        out.clusterOf   = std::move(clOf);
        out.threadCount = static_cast<int>(clusters.size());
        return out;
    };

    // ---- "All" composition (also produces the items[] thread IDs) -
    OneLinerBuild allBuild = composeOneLinerFromBag(ranked);
    snap.threadCount       = allBuild.threadCount;
    std::vector<int>& clusterOf = allBuild.clusterOf;
    if (clusterOf.empty()) clusterOf.assign(ranked.size(), -1);
    snap.model = m_modelReady ? m_modelName : std::string("deterministic");

    // ---- Structured breakdown (per-app top 5) ---------------------
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
        it.threadId     = (i < clusterOf.size() && clusterOf[i] >= 0)
                              ? clusterOf[i] + 1
                              : 0;
        snap.items.push_back(std::move(it));
    }

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

    snap.oneLiner = allBuild.text;

    // -----------------------------------------------------------------
    // Per-category one-liners.  Each is an *independent* projection of
    // the same 15-min activity window, composed through the same
    // pipeline above.  A consumer picks one of {all, documents,
    // websites, apps} via the dropdown to narrow the surface.
    //
    // Categorization rules:
    //   * Browsers (isBrowser=true) feed Websites only.
    //   * Document apps -- code editors, IDEs, Office, PDF readers,
    //     note apps, design tools that edit content -- feed Documents
    //     only.
    //   * Everything else (comms, terminals, media players, remote
    //     desktop, system tools) feeds Apps only.
    // Documents/Websites/Apps therefore form a strict partition of
    // ranked; an app that is in one is never in another.
    // -----------------------------------------------------------------
    // Categorize ranked entries into Documents vs Apps.  Browsers feed
    // Websites only.  Documents is an explicit whitelist of editor /
    // viewer / authoring app friendly names (see `IsDocumentFriendlyName`
    // near the top of the file) — *not* a verb-pattern match, because
    // the catch-all fallback verb for unrecognized apps is "Working
    // in" which would otherwise sweep WhatsApp UWP, MAUI chat apps,
    // and any other unknown app into Documents.
    auto isDocumentApp = [](const AppAgg& a) -> bool {
        if (a.isBrowser) return false;
        return IsDocumentFriendlyName(a.friendlyName);
    };

    std::vector<AppAgg> rankedDocs;
    std::vector<AppAgg> rankedApps;
    rankedDocs.reserve(ranked.size());
    rankedApps.reserve(ranked.size());
    for (const auto& a : ranked)
    {
        if (a.isBrowser) continue;
        if (isDocumentApp(a)) rankedDocs.push_back(a);
        else                  rankedApps.push_back(a);
    }

    // Augment Documents with recently-touched file basenames captured
    // by FileMonitor.  This catches docs that the user looked at
    // briefly (so they never accumulated significant focus seconds in
    // an editor) but that still represent a "document the user was
    // on".  Skip noisy extensions (.dll/.log/.tmp/.bak) and any
    // basenames already present in rankedDocs (case-insensitive).
    {
        std::unordered_set<std::string> seenLowered;
        for (const auto& a : rankedDocs)
            seenLowered.insert(AsciiLower(a.bestTitle));

        struct FileVirt { std::string base; int64_t ts; int hits; };
        std::unordered_map<std::string, FileVirt> byBase;
        for (const auto& f : files)
        {
            std::string p = WideToUtf8(f.path);
            if (p.empty()) continue;
            size_t slash = p.find_last_of("/\\");
            std::string base = (slash != std::string::npos) ? p.substr(slash + 1) : p;
            if (base.empty()) continue;
            std::string baseLower = AsciiLower(base);
            if (baseLower.size() >= 4 &&
                (baseLower.compare(baseLower.size()-4, 4, ".dll") == 0 ||
                 baseLower.compare(baseLower.size()-4, 4, ".log") == 0 ||
                 baseLower.compare(baseLower.size()-4, 4, ".tmp") == 0 ||
                 baseLower.compare(baseLower.size()-4, 4, ".bak") == 0))
                continue;
            if (seenLowered.count(baseLower)) continue;
            auto it = byBase.find(baseLower);
            if (it == byBase.end())
                byBase[baseLower] = { base, f.timestampUtc, 1 };
            else
            {
                it->second.hits += 1;
                if (f.timestampUtc > it->second.ts) it->second.ts = f.timestampUtc;
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
            agg.friendlyName    = "Document";
            agg.verb            = "Editing";
            agg.bestTitle       = v.base;
            agg.totalFocusSecs  = (std::max)(5, v.hits * 5);
            agg.lastSeenTs      = v.ts;
            agg.isBrowser       = false;
            rankedDocs.push_back(std::move(agg));
        }
        std::sort(rankedDocs.begin(), rankedDocs.end(),
                  [](const AppAgg& a, const AppAgg& b){
                      if (a.totalFocusSecs != b.totalFocusSecs)
                          return a.totalFocusSecs > b.totalFocusSecs;
                      return a.lastSeenTs > b.lastSeenTs;
                  });
    }

    // Build the Websites bag from BrowsingMonitor records.  Each
    // unique tab title becomes a virtual AppAgg with focus weighted
    // by visit count (we don't have per-tab dwell time -- visit count
    // is a reasonable proxy for "interest").  Cap at 16 entries so we
    // don't blow ONNX cost on hyper-active browsing windows.
    std::vector<AppAgg> rankedWeb;
    {
        std::unordered_map<std::string, AppAgg> webByTitle;
        for (const auto& b : browse)
        {
            std::string raw = WideToUtf8(b.title);
            if (raw.empty()) continue;
            std::string clean = CleanTitle(raw, "Browser");
            if (clean.empty()) continue;
            std::string key = AsciiLower(clean);
            auto& agg = webByTitle[key];
            if (agg.bestTitle.empty())
            {
                agg.bestTitle    = clean;
                agg.friendlyName = "Browser";
                agg.verb         = "Reading";
                agg.isBrowser    = true;
            }
            agg.totalFocusSecs += 30;
            if (b.timestampUtc > agg.lastSeenTs) agg.lastSeenTs = b.timestampUtc;
        }
        rankedWeb.reserve(webByTitle.size());
        for (auto& kv : webByTitle) rankedWeb.push_back(std::move(kv.second));
        std::sort(rankedWeb.begin(), rankedWeb.end(),
                  [](const AppAgg& a, const AppAgg& b){
                      if (a.totalFocusSecs != b.totalFocusSecs)
                          return a.totalFocusSecs > b.totalFocusSecs;
                      return a.lastSeenTs > b.lastSeenTs;
                  });
        if (rankedWeb.size() > 16) rankedWeb.resize(16);
    }

    snap.oneLinerDocuments = composeOneLinerFromBag(rankedDocs, /*categoryMode=*/true).text;
    snap.oneLinerWebsites  = composeOneLinerFromBag(rankedWeb,  /*categoryMode=*/true).text;
    snap.oneLinerApps      = composeOneLinerFromBag(rankedApps, /*categoryMode=*/true).text;

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
                                                   const std::string& category)
{
    // Resolve the top-level "one_liner" surface based on the requested
    // category.  "all" (default) keeps the combined one; the three
    // category names swap in the matching per-category line so a UI
    // that simply renders the JSON sees a focused response.  All four
    // category-specific fields are *also* emitted for consumers that
    // want the full breakdown.
    std::string topLine = s.oneLiner;
    if (category == "documents") topLine = s.oneLinerDocuments;
    else if (category == "websites") topLine = s.oneLinerWebsites;
    else if (category == "apps")     topLine = s.oneLinerApps;

    std::ostringstream o;
    o << "{"
      << "\"timestamp\":"       << s.timestamp
      << ",\"window_start\":"   << s.windowStartTs
      << ",\"window_end\":"     << s.windowEndTs
      << ",\"category\":\""     << EscapeJson(category) << "\""
      << ",\"one_liner\":\""    << EscapeJson(topLine) << "\""
      << ",\"one_liner_all\":\""       << EscapeJson(s.oneLiner)          << "\""
      << ",\"one_liner_documents\":\"" << EscapeJson(s.oneLinerDocuments) << "\""
      << ",\"one_liner_websites\":\""  << EscapeJson(s.oneLinerWebsites)  << "\""
      << ",\"one_liner_apps\":\""      << EscapeJson(s.oneLinerApps)      << "\""
      << ",\"activity_count\":" << s.activityCount
      << ",\"focus_seconds\":"  << s.focusSeconds
      << ",\"dominant_focus_pct\":" << s.dominantPct
      << ",\"confidence\":"     << s.confidence
      << ",\"thread_count\":"   << s.threadCount
      << ",\"model\":\""        << EscapeJson(s.model) << "\""
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
          << ",\"pct\":" << it.pct
          << ",\"thread_id\":" << it.threadId << "}";
    }
    o << "]}";
    return o.str();
}

// Normalize the requested category to one of the four known values.
// Empty / unknown / "all" all map to "all".
static std::string NormalizeCategory(const std::string& c)
{
    if (c == "documents" || c == "websites" || c == "apps") return c;
    return "all";
}

std::string ContextInference::GetRecentContext(const std::string& category)
{
    std::string cat = NormalizeCategory(category);
    std::lock_guard<std::mutex> lk(m_mutex);
    std::ostringstream o;
    o << "{\"recent_context\":";
    if (m_haveLatest)
        o << SnapshotToJsonObject(m_latest, cat);
    else
        o << "null";
    o << ",\"category\":\"" << cat << "\""
      << ",\"history_count\":" << m_history.size() << "}";
    return o.str();
}

std::string ContextInference::GetRecentContexts(int count, const std::string& category)
{
    if (count <= 0)  count = 10;
    if (count > 200) count = 200;
    std::string cat = NormalizeCategory(category);

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
        o << SnapshotToJsonObject(*latestFirst[i], cat);
    }
    o << "],\"category\":\"" << cat << "\""
      << ",\"returned\":" << latestFirst.size()
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
