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

// MiniLM embedding pipeline constants.
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
//  emerges from the actual titles.  MiniLM is used (when available) to
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
//  Init -- load MiniLM model + tokenizer if available.  This is best-effort:
//  if any step fails the engine still runs in deterministic-only mode.
// =====================================================================
bool ContextInference::Init(const std::wstring& modelsDir)
{
    if (modelsDir.empty()) return true;  // explicitly skip model load

    // Tokenizer vocab
    std::string vocabPath = WideToUtf8(modelsDir) + "\\vocab.txt";
    if (!m_tokenizer.Load(vocabPath))
        return true;  // graceful degrade

    std::wstring modelPath = modelsDir + L"\\minilm.onnx";

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
    return true;
}

// =====================================================================
//  Sentence embedding with MiniLM (384-dim, mean-pool + L2-normalize).
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

    // ---- One-liner composition ------------------------------------
    auto phraseFor = [](const AppAgg& a) -> std::string {
        if (a.bestTitle.empty())
            return std::string(a.verb) + " " + a.friendlyName;
        return std::string(a.verb) + " \"" + a.bestTitle + "\" in " + a.friendlyName;
    };

    // ---- Dynamic semantic clustering (MiniLM) ---------------------
    // For each ranked app, embed its candidate phrase and run greedy
    // clustering with cosine similarity >= CLUSTER_COS_THRESHOLD.  Apps
    // whose embeddings are close get merged into one "thread".  This
    // surfaces, e.g., editing `auth.cpp` and viewing the auth PR as a
    // single thread instead of two unrelated lines.
    //
    // If the model is not loaded, every app becomes its own cluster
    // (== the previous per-app composition).
    std::vector<int>                 clusterOf(ranked.size(), -1);
    std::vector<std::vector<float>>  clusterCentroid; // 384-dim each
    std::vector<int>                 clusterTotalFocus;
    std::vector<std::vector<size_t>> clusterMembers;  // indices into `ranked`

    if (m_modelReady && ranked.size() >= 2)
    {
        // Cap embedding work at top 8 to bound CPU on hyper-noisy users.
        const size_t embedLimit = (std::min)(ranked.size(), static_cast<size_t>(8));
        std::vector<std::vector<float>> appEmb(ranked.size());
        for (size_t i = 0; i < embedLimit; ++i)
            appEmb[i] = Embed(phraseFor(ranked[i]));

        for (size_t i = 0; i < ranked.size(); ++i)
        {
            if (i >= embedLimit || appEmb[i].empty())
            {
                // No embedding available -- give it its own cluster.
                int cid = static_cast<int>(clusterCentroid.size());
                clusterCentroid.push_back({});
                clusterTotalFocus.push_back(ranked[i].totalFocusSecs);
                clusterMembers.push_back({ i });
                clusterOf[i] = cid;
                continue;
            }
            int bestC = -1;
            float bestSim = CLUSTER_COS_THRESHOLD;
            for (size_t c = 0; c < clusterCentroid.size(); ++c)
            {
                if (clusterCentroid[c].empty()) continue;
                float s = CosineSim(appEmb[i], clusterCentroid[c]);
                if (s > bestSim)
                {
                    bestSim = s;
                    bestC = static_cast<int>(c);
                }
            }
            if (bestC < 0)
            {
                int cid = static_cast<int>(clusterCentroid.size());
                clusterCentroid.push_back(appEmb[i]);
                clusterTotalFocus.push_back(ranked[i].totalFocusSecs);
                clusterMembers.push_back({ i });
                clusterOf[i] = cid;
            }
            else
            {
                // Merge into existing cluster: re-average centroid.
                std::vector<float>& c = clusterCentroid[bestC];
                size_t n = clusterMembers[bestC].size();
                for (int d = 0; d < EMBED_DIM; ++d)
                    c[d] = (c[d] * static_cast<float>(n) + appEmb[i][d])
                           / static_cast<float>(n + 1);
                // Re-normalize centroid.
                float norm = 0.0f;
                for (int d = 0; d < EMBED_DIM; ++d) norm += c[d] * c[d];
                norm = std::sqrt(norm);
                if (norm > 1e-9f)
                    for (int d = 0; d < EMBED_DIM; ++d) c[d] /= norm;

                clusterMembers[bestC].push_back(i);
                clusterTotalFocus[bestC] += ranked[i].totalFocusSecs;
                clusterOf[i] = bestC;
            }
        }
        snap.model = "all-MiniLM-L6-v2";
    }
    else
    {
        // No model -- every app is its own cluster.
        for (size_t i = 0; i < ranked.size(); ++i)
        {
            clusterCentroid.push_back({});
            clusterTotalFocus.push_back(ranked[i].totalFocusSecs);
            clusterMembers.push_back({ i });
            clusterOf[i] = static_cast<int>(i);
        }
        snap.model = "deterministic";
    }

    snap.threadCount = static_cast<int>(clusterMembers.size());

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
        it.threadId     = clusterOf[i] + 1; // 1-based for callers
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

    // ---- Compose one-liner from cluster representatives -----------
    // Sort clusters by total focus desc, picking each cluster's
    // representative (highest-focus member -- which is also the first
    // member, since `ranked` is already sorted by focus).
    struct Cluster
    {
        int    totalFocus;
        size_t repIdx;             // index into ranked
        std::vector<size_t> members;
    };
    std::vector<Cluster> clusters;
    clusters.reserve(clusterMembers.size());
    for (size_t c = 0; c < clusterMembers.size(); ++c)
    {
        Cluster cl;
        cl.totalFocus = clusterTotalFocus[c];
        cl.members    = clusterMembers[c];
        // Rep = first member (already focus-sorted).
        cl.repIdx     = clusterMembers[c].front();
        clusters.push_back(std::move(cl));
    }
    std::sort(clusters.begin(), clusters.end(),
              [](const Cluster& a, const Cluster& b){
                  return a.totalFocus > b.totalFocus;
              });

    // ---- Semantic theme extraction (per cluster) ------------------
    // For each cluster: gather content tokens from the cleaned titles
    // of its members, score them (frequency * (1 + cosine-to-clean-
    // centroid) when MiniLM is loaded, frequency-only otherwise), and
    // pick the top 1-2 as the cluster's *theme phrase*.  This produces
    // a semantic 1-liner -- "Working on authentication", not "Editing
    // \"auth.cpp - WARP\" in Visual Studio".  Fallback to verbatim
    // title when no usable content tokens remain.
    auto themeFor = [&](const Cluster& cl) -> std::string {
        // Bag of tokens across this cluster's member titles.
        std::unordered_map<std::string, int>          freq;
        std::unordered_map<std::string, std::string>  bestSurface; // case-pretty form
        std::vector<std::string>                      orderedLowers;

        for (size_t mi : cl.members)
        {
            auto toks = ExtractContentTokens(ranked[mi].bestTitle);
            for (const auto& t : toks)
            {
                if (freq.find(t.lower) == freq.end())
                    orderedLowers.push_back(t.lower);
                freq[t.lower] += 1;
                // Prefer the title-cased / mixed-case original spelling
                // (e.g. "Authentication" beats "authentication").
                auto& s = bestSurface[t.lower];
                if (s.empty() ||
                   (s.size() == t.surface.size() && t.surface > s)) // crude tiebreak
                    s = t.surface;
            }
        }
        if (orderedLowers.empty()) return std::string();

        struct ScoredTok { std::string lower; double score; size_t firstIdx; };
        std::vector<ScoredTok> scored;
        scored.reserve(orderedLowers.size());

        // Build a "clean centroid" from the joined content tokens of
        // this cluster's titles.  Only used when MiniLM is loaded.
        std::vector<float> cleanCentroid;
        if (m_modelReady)
        {
            std::string joined;
            for (const auto& lc : orderedLowers)
            {
                if (!joined.empty()) joined.push_back(' ');
                // Use the surface form so casing can help the encoder.
                joined += bestSurface[lc].empty() ? lc : bestSurface[lc];
            }
            cleanCentroid = Embed(joined);
        }

        // Cap embedded candidates to top 8 by raw frequency to keep
        // ONNX cost bounded.  Ties broken by first-seen order.
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
                    // Keep cosine bounded to [0,1]+ so the multiplier
                    // never goes negative; semantically irrelevant
                    // tokens get score >= freq, central ones get more.
                    if (cs < 0.0f) cs = 0.0f;
                    s *= (1.0 + static_cast<double>(cs));
                }
            }
            // First-seen index (lower = appears earlier in titles).
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

        // Pick top 1-2 tokens; drop the second if it's much weaker
        // (< 60 % of the top score) or duplicates the first as a prefix.
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

        // Order the picks by the position they first appeared in titles
        // so the phrase reads naturally ("Context Inference" not
        // "Inference Context").
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

        std::string out;
        for (const auto& p : picks) { if (!out.empty()) out += " "; out += p; }
        return TitleCase(out);
    };

    // Pick a semantic verb based on the rep app's verb + the dominant
    // content tokens in the cluster.  Keeps the verb set small so the
    // one-liner stays uniform.
    auto verbFor = [&](const Cluster& cl, const std::string& theme) -> std::string {
        const AppAgg& rep = ranked[cl.repIdx];
        std::string repVerb = AsciiLower(rep.verb);
        std::string lowerTheme = AsciiLower(theme);

        auto themeHas = [&](const char* kw) -> bool {
            return lowerTheme.find(kw) != std::string::npos;
        };
        // Look across ALL member titles for keyword hints (cleaned form).
        auto anyTitleHas = [&](const char* kw) -> bool {
            for (size_t mi : cl.members)
            {
                std::string lo = AsciiLower(ranked[mi].bestTitle);
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
        // Fallback: keep the rep's verb but normalise capitalization.
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
            // No usable content tokens -- fall back to the verbatim
            // per-app phrase to avoid emitting a context-free verb.
            p = phraseFor(ranked[cl.repIdx]);
        }
        else
        {
            std::string verb = verbFor(cl, theme);
            p = verb + " " + theme;
        }

        // Append app-context tail when this cluster spans 2+ apps so
        // the user can still see *where* the work is happening.
        if (cl.members.size() >= 2)
        {
            std::string suffix;
            size_t shown = 0;
            for (size_t j = 0; j < cl.members.size() && shown < 3; ++j, ++shown)
            {
                if (shown == 0)
                    suffix = " (across " + ranked[cl.members[j]].friendlyName;
                else if (shown + 1 == (std::min)(cl.members.size(), size_t{3}))
                    suffix += " & " + ranked[cl.members[j]].friendlyName;
                else
                    suffix += ", " + ranked[cl.members[j]].friendlyName;
            }
            size_t hidden = cl.members.size() - shown;
            if (hidden > 0)
            {
                std::ostringstream o;
                o << " + " << hidden << " more";
                suffix += o.str();
            }
            suffix += ")";
            p += suffix;
        }
        return p;
    };

    // Adaptive top-N: include clusters until we cover ~80 % of focus
    // OR the character budget is hit.  Always include at least one.
    std::string oneLiner;
    int covered = 0;
    size_t included = 0;
    for (size_t i = 0; i < clusters.size(); ++i)
    {
        std::string phrase = clusterPhrase(clusters[i]);
        std::string candidate = oneLiner.empty()
            ? phrase
            : oneLiner + " \xC2\xB7 " + phrase; // " · "
        if (i > 0
         && (candidate.size() > ONE_LINER_BUDGET
          || (totalFocusSecs > 0
              && static_cast<int>((covered * 100LL) / totalFocusSecs) >= 80)))
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
