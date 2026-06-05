#include "framework.h"
#include "LlmSummarizer.h"

#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <chrono>

#ifdef WARP_HAS_ORT_GENAI
#include <ort_genai_c.h>
#endif

// =====================================================================
//  Tunables
// =====================================================================
namespace
{
    // Max tokens the LLM is allowed to produce.  Each summary line is
    // ~10-20 tokens, capped at 3 lines => ~60 tokens is generous.
    constexpr int    kMaxNewTokens         = 96;

    // Hard timeout per Polish() call.  500ms-2s is the expected band;
    // 5s gives plenty of headroom while still bounded.
    constexpr int    kPolishTimeoutMs      = 5000;

    // Max chars per summary line in the polished output.  Anything
    // longer is rejected (LLM lost the plot).
    constexpr size_t kMaxPolishedLineLen   = 160;

    // Max number of items we include in the prompt grounding facts.
    // The composer already caps `items[]` at 5; matches that.
    constexpr size_t kMaxPromptItems       = 5;

    // Max chars per item title in the prompt -- avoid blowing the
    // context window on a single huge title.
    constexpr size_t kMaxPromptTitleLen    = 100;

    constexpr const char*    kModelName        = "qwen3-0.6b";
    constexpr const wchar_t* kModelSubDir      = L"\\qwen";
}

// =====================================================================
//  Impl -- holds the ORT-GenAI session.  PIMPL keeps the header free
//  of onnxruntime-genai includes so callers don't need that dependency.
// =====================================================================
struct LlmSummarizer::Impl
{
#ifdef WARP_HAS_ORT_GENAI
    OgaModel*     model     = nullptr;
    OgaTokenizer* tokenizer = nullptr;
#endif
    std::wstring modelDir;
};

LlmSummarizer::LlmSummarizer()
    : m_impl(std::make_unique<Impl>())
{
}

LlmSummarizer::~LlmSummarizer()
{
    Stop();
}

std::string LlmSummarizer::ModelName() const
{
    return m_ready.load() ? std::string(kModelName)
                          : std::string("(not loaded)");
}

// ---------------------------------------------------------------------
//  Init -- looks for `<modelsDir>\qwen\genai_config.json` and loads
//  the model + tokenizer through onnxruntime-genai.  Best effort:
//  any failure (missing files, ORT-GenAI absent at build time, init
//  exception) results in a clean return-false with no side effects.
// ---------------------------------------------------------------------
bool LlmSummarizer::Init(const std::wstring& modelsDir)
{
    Stop();

    if (modelsDir.empty()) return false;

    std::wstring qwenDir = modelsDir + kModelSubDir;
    std::wstring cfgPath = qwenDir + L"\\genai_config.json";
    if (GetFileAttributesW(cfgPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;   // model not present; quietly stay disabled

#ifdef WARP_HAS_ORT_GENAI
    try
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, qwenDir.c_str(), -1,
                                      nullptr, 0, nullptr, nullptr);
        if (len <= 0) return false;
        std::string narrow(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, qwenDir.c_str(), -1,
                            &narrow[0], len, nullptr, nullptr);

        OgaResult* res = OgaCreateModel(narrow.c_str(), &m_impl->model);
        if (res || !m_impl->model)
        {
            if (res) OgaDestroyResult(res);
            return false;
        }
        res = OgaCreateTokenizer(m_impl->model, &m_impl->tokenizer);
        if (res || !m_impl->tokenizer)
        {
            if (res) OgaDestroyResult(res);
            OgaDestroyModel(m_impl->model);
            m_impl->model = nullptr;
            return false;
        }
        m_impl->modelDir = std::move(qwenDir);
        m_ready.store(true);
        return true;
    }
    catch (...)
    {
        if (m_impl->tokenizer) { OgaDestroyTokenizer(m_impl->tokenizer); m_impl->tokenizer = nullptr; }
        if (m_impl->model)     { OgaDestroyModel(m_impl->model);         m_impl->model     = nullptr; }
        return false;
    }
#else
    // Built without ORT-GenAI -- LLM polishing not available.
    (void)cfgPath;
    return false;
#endif
}

void LlmSummarizer::Stop()
{
    m_ready.store(false);
#ifdef WARP_HAS_ORT_GENAI
    if (m_impl->tokenizer) { OgaDestroyTokenizer(m_impl->tokenizer); m_impl->tokenizer = nullptr; }
    if (m_impl->model)     { OgaDestroyModel(m_impl->model);         m_impl->model     = nullptr; }
#endif
    m_impl->modelDir.clear();
}

// =====================================================================
//  Prompt construction
// =====================================================================
namespace
{
    // Truncate to a max byte length; if truncated, append a single '.'.
    // (UTF-8 trailing bytes are not preserved; the LLM is robust to
    // mildly malformed sequences in a prompt.)
    std::string Truncate(const std::string& s, size_t maxLen)
    {
        if (s.size() <= maxLen) return s;
        return s.substr(0, maxLen) + ".";
    }

    // Build the chat-formatted prompt for Qwen3.
    // The model's tokenizer adds its own special tokens via
    // ApplyChatTemplate (when supported); we just hand it the
    // structured conversation.  For portability across ORT-GenAI
    // versions we emit the raw chat template ourselves with the
    // <|im_start|> / <|im_end|> markers Qwen2.5 and Qwen3 both expect.
    std::string BuildPrompt(const std::vector<std::string>& existing,
                            const std::vector<LlmActivityItem>& items,
                            const std::string& category)
    {
        // Topic-first prompt design.  The job is NOT to describe which
        // apps are open ("Outlook is open, Edge is running") -- that
        // wording is exactly what previous revisions produced and the
        // user called out as useless.  The job is to capture WHAT the
        // user is working on -- the subject / topic that connects the
        // open windows -- with apps appearing only as a brief
        // grounding clause at the end.  The window TITLES carry the
        // topic signal; framing items as "<title> (<app>)" with the
        // title first nudges the model to anchor on titles rather
        // than apps.
        //
        // The template-composed `existing` summary is re-included as
        // an explicit topic hint (the ContextInference clustering
        // pipeline already does the cross-app theme distillation; the
        // LLM's job is to phrase it naturally and ground it in the
        // titles).  Anti-copy is enforced post-generation by
        // IsNearCopyOfExisting (verbatim copies are rejected; natural
        // rewrites are kept).
        //
        // The three concrete examples in the system prompt are
        // critical: Qwen3-0.6B is small enough that a few-shot
        // pattern in the system message dramatically improves output
        // quality compared to rule-only instruction.
        std::ostringstream sys;
        sys << "You read a snapshot of the window titles on someone's "
               "screen and write 1 to 3 short sentences describing "
               "WHAT THEY ARE WORKING ON -- the subject or topic of "
               "their work, not which apps they have open.\n\n"
               "Style rules:\n"
               "- Start with \"User is\" (or \"They are\") followed "
               "by what they are doing.\n"
               "- Lead with the TOPIC. Mention apps only briefly at "
               "the end, e.g. \"...in their emails and chats\" or "
               "\"...across Outlook and Teams\".\n"
               "- If multiple titles share a theme, capture that "
               "theme; do not list every window.\n"
               "- Do NOT say \"is open\", \"is running\", \"is "
               "using\".\n"
               "- Plain English. No bullets, no numbered lists, no "
               "markdown, no quotes around the output.\n\n"
               "Examples of the target style:\n"
               "  User is reading about indexer reliability across "
               "their emails and chats (Outlook, Microsoft Teams).\n"
               "  User is reviewing the auth refactor PR in their "
               "browser and editing the related source file in "
               "Visual Studio.\n"
               "  User is exploring various websites about React "
               "hooks and state management.";

        if (category == "files")
            sys << " The snapshot covers documents and files only.";
        else if (category == "websites")
            sys << " The snapshot covers web pages only.";
        else if (category == "apps")
            sys << " The snapshot covers communication and utility "
                   "apps only.";

        std::ostringstream user;
        user << "Window titles on the user's screen (most-used first):\n";
        size_t shown = 0;
        for (const auto& it : items)
        {
            if (shown >= kMaxPromptItems) break;
            const std::string& title =
                it.title.empty() ? it.rawTitle : it.title;
            // Title-first ordering: nudges the model to anchor on the
            // topic-carrying string rather than the generic app name.
            user << "- \""
                 << Truncate(title, kMaxPromptTitleLen)
                 << "\" (" << it.app << ")\n";
            ++shown;
        }
        if (shown == 0) user << "- (nothing active)\n";

        if (!existing.empty())
        {
            user << "\nTopic hint already extracted from the titles "
                    "(use as inspiration, do NOT copy verbatim):\n";
            for (const auto& line : existing)
                user << "  " << line << "\n";
        }

        user << "\nWrite 1-3 sentences describing what they are "
                "working on:";

        // Qwen chat-template format (same for Qwen2.5 and Qwen3).
        //
        // Qwen3 ships a built-in chain-of-thought "thinking" mode that
        // is enabled by default; when we hand-build the prompt the
        // model will happily emit a long <think>...</think> reasoning
        // block before the actual answer, which (a) consumes our 96-
        // token budget on reasoning instead of the summary itself, so
        // the answer is never reached, and (b) leaks raw reasoning
        // ("First, I need to extract the information from the user's
        // input...") into summary_polished.  The official Qwen3
        // chat-template handles this by pre-pending an empty
        // <think></think> block after the assistant header when
        // `enable_thinking=False`; replicate that pattern here so the
        // model skips reasoning and goes straight to the summary.
        std::ostringstream prompt;
        prompt << "<|im_start|>system\n" << sys.str() << "<|im_end|>\n"
               << "<|im_start|>user\n"   << user.str() << "<|im_end|>\n"
               << "<|im_start|>assistant\n"
               << "<think>\n\n</think>\n\n";
        return prompt.str();
    }
}

// =====================================================================
//  Post-processing -- split, trim, validate
// =====================================================================
namespace
{
    std::string TrimAscii(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (unsigned char)s[a] <= ' ') ++a;
        while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
        return s.substr(a, b - a);
    }

    // Strip common LLM artifacts: leading "- ", "* ", "1. " etc.,
    // surrounding quotes, trailing "**", markdown emphasis stars.
    std::string CleanLine(std::string s)
    {
        s = TrimAscii(s);
        // Strip a leading bullet / number.
        if (s.size() >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == ':'))
        {
            size_t i = 1;
            while (i < s.size() && s[i] == ' ') ++i;
            s = s.substr(i);
        }
        else if (s.size() >= 3 && std::isdigit((unsigned char)s[0])
              && (s[1] == '.' || s[1] == ')') && s[2] == ' ')
        {
            s = s.substr(3);
        }
        // Strip a single pair of surrounding double-quotes.
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
        // Strip markdown emphasis stars.
        while (s.size() >= 2 && s.front() == '*') s.erase(0, 1);
        while (s.size() >= 2 && s.back()  == '*') s.pop_back();
        return TrimAscii(s);
    }

    // Returns true if `line` is obviously a parroted fragment of the
    // input prompt (the activity-facts list or the instruction text)
    // rather than a real summary sentence.  Kept narrow on purpose:
    // the previous revision was rejecting too aggressively (e.g. any
    // line opening "App: \"...\"") and dropped legitimate output.
    bool IsPromptEcho(const std::string& line)
    {
        // Lowercase ASCII view for substring matching.
        std::string lc;
        lc.reserve(line.size());
        for (char c : line)
            lc.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));

        // Genuine instruction-text leakage from any prompt revision
        // we've shipped.  These phrases would essentially never appear
        // in a natural summary sentence.
        if (lc.find("(1-3 lines")          != std::string::npos) return true;
        if (lc.find("(1 to 3 sentence")    != std::string::npos) return true;
        if (lc.find("(1 to 3 lines")       != std::string::npos) return true;
        if (lc.find("refined summary")     != std::string::npos) return true;
        if (lc.find("candidate summary")   != std::string::npos) return true;
        if (lc.find("draft notes")         != std::string::npos) return true;
        if (lc.find("polished prose")      != std::string::npos) return true;
        if (lc.find("most-used first")     != std::string::npos) return true;
        if (lc.find("most-focused first")  != std::string::npos) return true;
        if (lc.find("activity facts")      != std::string::npos) return true;
        if (lc.find("apps and windows open right now") != std::string::npos)
            return true;

        // Legacy structured-item echoes.
        if (lc.find("% of focus")          != std::string::npos) return true;
        if (lc.find("[major]")             != std::string::npos) return true;
        if (lc.find("[present]")           != std::string::npos) return true;
        if (lc.find("[minor]")             != std::string::npos) return true;
        if (lc.find("app=")   != std::string::npos &&
            lc.find("title=") != std::string::npos)
            return true;

        return false;
    }

    // Normalize a line for "is this a copy of an existing summary
    // line" comparison: lowercase, strip non-alphanumeric, collapse
    // whitespace.  Used to detect the "model copied the candidate
    // summary back verbatim" failure mode.
    std::string NormalizeForCompare(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        bool lastSpace = true;
        for (char c : s)
        {
            unsigned char u = static_cast<unsigned char>(c);
            if (std::isalnum(u))
            {
                out.push_back(static_cast<char>(std::tolower(u)));
                lastSpace = false;
            }
            else if (!lastSpace)
            {
                out.push_back(' ');
                lastSpace = true;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }

    // True if `line` is essentially a verbatim restatement of one of
    // the `existing` summary lines, i.e. the model just echoed the
    // topic hint we provided in the prompt instead of writing fresh
    // prose.  Tightened from the previous revision (which used a
    // substring check) so that legitimate natural rewrites are kept:
    // the new topic-first prompt deliberately invites the model to
    // rephrase the existing topic hint in fresh English (adding
    // "User is", grounding it in apps), and a strict substring check
    // would falsely reject such rewrites.  We now reject only:
    //   1. exact normalized equality, OR
    //   2. exact normalized equality after stripping a leading
    //      "user is " / "they are " / "currently " from the polished
    //      line (these openers are added by the model template but
    //      add zero information beyond the existing line).
    bool IsNearCopyOfExisting(const std::string& line,
                              const std::vector<std::string>& existing)
    {
        if (existing.empty()) return false;
        std::string nl = NormalizeForCompare(line);
        if (nl.empty()) return false;

        // Strip a leading discourse marker if present.
        auto stripOpener = [](const std::string& s) -> std::string {
            static const char* const openers[] = {
                "user is ", "they are ", "the user is ",
                "currently ", "right now ",
            };
            for (const char* op : openers)
            {
                size_t opLen = std::strlen(op);
                if (s.size() > opLen &&
                    s.compare(0, opLen, op) == 0)
                    return s.substr(opLen);
            }
            return s;
        };
        std::string nlOpenerless = stripOpener(nl);

        for (const auto& e : existing)
        {
            std::string ne = NormalizeForCompare(e);
            if (ne.empty()) continue;
            if (nl == ne)            return true;
            if (nlOpenerless == ne)  return true;
        }
        return false;
    }

    std::vector<std::string> SplitIntoLines(const std::string& text,
                                            size_t maxLines)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start < text.size() && out.size() < maxLines)
        {
            size_t nl = text.find('\n', start);
            std::string raw =
                (nl == std::string::npos)
                ? text.substr(start)
                : text.substr(start, nl - start);
            std::string line = CleanLine(raw);
            if (!line.empty() && line.size() <= kMaxPolishedLineLen
                && !IsPromptEcho(line))
            {
                // De-dup against earlier lines (case-insensitive).
                bool dup = false;
                for (const auto& prev : out)
                {
                    if (prev.size() == line.size())
                    {
                        bool eq = true;
                        for (size_t k = 0; k < prev.size(); ++k)
                            if (std::tolower((unsigned char)prev[k]) !=
                                std::tolower((unsigned char)line[k]))
                            { eq = false; break; }
                        if (eq) { dup = true; break; }
                    }
                }
                if (!dup) out.push_back(std::move(line));
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        return out;
    }
}

// =====================================================================
//  Polish
// =====================================================================
std::vector<std::string>
LlmSummarizer::Polish(const std::vector<std::string>& existing,
                      const std::vector<LlmActivityItem>& items,
                      const std::string& category) const
{
    if (!m_ready.load()) return {};
    if (items.empty() && existing.empty()) return {};

#ifdef WARP_HAS_ORT_GENAI
    std::lock_guard<std::mutex> lk(m_genMutex);

    // RAII helpers for the C API objects we'll allocate.
    OgaSequences*        promptSeqs = nullptr;
    OgaGeneratorParams*  params     = nullptr;
    OgaGenerator*        generator  = nullptr;
    const char*          decodedStr = nullptr;
    auto cleanup = [&]() {
        if (decodedStr) { OgaDestroyString(decodedStr);          decodedStr = nullptr; }
        if (generator)  { OgaDestroyGenerator(generator);        generator  = nullptr; }
        if (params)     { OgaDestroyGeneratorParams(params);     params     = nullptr; }
        if (promptSeqs) { OgaDestroySequences(promptSeqs);       promptSeqs = nullptr; }
    };
    auto failed = [&](OgaResult* r) -> bool {
        if (r) { OgaDestroyResult(r); cleanup(); return true; }
        return false;
    };

    try
    {
        std::string promptText = BuildPrompt(existing, items, category);

        if (failed(OgaCreateSequences(&promptSeqs))) return {};
        if (failed(OgaTokenizerEncode(m_impl->tokenizer,
                                      promptText.c_str(),
                                      promptSeqs))) return {};

        if (failed(OgaCreateGeneratorParams(m_impl->model, &params))) return {};
        size_t promptLen = OgaSequencesGetSequenceCount(promptSeqs, 0);
        if (failed(OgaGeneratorParamsSetSearchNumber(
                params, "max_length",
                static_cast<double>(promptLen + kMaxNewTokens))))
            return {};
        // temperature is honored only when do_sample is true; default
        // is greedy decoding which produces stable outputs across
        // cycles -- a feature for a periodic context tool.
        OgaResult* ignore = OgaGeneratorParamsSetSearchNumber(
            params, "temperature", 0.2);
        if (ignore) OgaDestroyResult(ignore);   // swallow if option not supported

        if (failed(OgaCreateGenerator(m_impl->model, params, &generator))) return {};
        if (failed(OgaGenerator_AppendTokenSequences(generator, promptSeqs))) return {};

        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(kPolishTimeoutMs);

        while (!OgaGenerator_IsDone(generator))
        {
            if (std::chrono::steady_clock::now() > deadline) break;
            if (failed(OgaGenerator_GenerateNextToken(generator))) return {};
        }

        size_t sequenceLen = OgaGenerator_GetSequenceCount(generator, 0);
        if (sequenceLen <= promptLen) { cleanup(); return {}; }

        const int32_t* fullSeq = OgaGenerator_GetSequenceData(generator, 0);
        if (!fullSeq) { cleanup(); return {}; }
        size_t newCount = sequenceLen - promptLen;

        if (failed(OgaTokenizerDecode(m_impl->tokenizer,
                                      fullSeq + promptLen,
                                      newCount,
                                      &decodedStr))) return {};

        std::string decoded = decodedStr ? std::string(decodedStr) : std::string();
        cleanup();

        // Safety net: strip any <think>...</think> reasoning block
        // that the model still emits despite the no-think sentinel in
        // the prompt.  We strip the *paired* form first (open and
        // matching close tags), then fall back to truncating from any
        // residual <think> opener with no close (model started
        // thinking but ran out of token budget -- yields no usable
        // output, return empty later via the lines.empty() gate).
        for (;;)
        {
            size_t open  = decoded.find("<think>");
            if (open == std::string::npos) break;
            size_t close = decoded.find("</think>", open);
            if (close == std::string::npos)
            {
                decoded.erase(open);   // dangling <think> -- drop the rest
                break;
            }
            decoded.erase(open, (close + 8 /* len("</think>") */) - open);
        }
        // Also drop any orphan closing tag from a malformed emission.
        size_t orphanClose = decoded.find("</think>");
        if (orphanClose != std::string::npos)
            decoded.erase(0, orphanClose + 8);

        size_t imEnd = decoded.find("<|im_end|>");
        if (imEnd != std::string::npos) decoded.erase(imEnd);
        size_t imStart = decoded.find("<|im_start|>");
        if (imStart != std::string::npos) decoded.erase(imStart);

        auto lines = SplitIntoLines(decoded, 3);
        if (lines.empty()) return {};

        // Anti-copy gate: drop any line that is a near-verbatim copy
        // of an entry in `existing` (the template-composed summary
        // we'd otherwise be polishing).  This catches the
        // summary_polished == summary failure mode where the model
        // played it safe and copied a candidate line back unchanged.
        {
            std::vector<std::string> filtered;
            filtered.reserve(lines.size());
            for (auto& line : lines)
            {
                if (!IsNearCopyOfExisting(line, existing))
                    filtered.push_back(std::move(line));
            }
            lines = std::move(filtered);
            if (lines.empty()) return {};
        }

        // Last sanity gate: at least one line must mention a token
        // that appeared in the input items (grounding).  Otherwise we
        // assume the model hallucinated and reject.
        std::unordered_set<std::string> inputTokens;
        auto addLowerTokens = [&](const std::string& s) {
            std::string cur;
            for (char c : s)
            {
                unsigned char u = (unsigned char)c;
                if (std::isalnum(u))
                    cur.push_back((char)std::tolower(u));
                else
                {
                    if (cur.size() >= 4) inputTokens.insert(cur);
                    cur.clear();
                }
            }
            if (cur.size() >= 4) inputTokens.insert(cur);
        };
        for (const auto& it : items)
        {
            addLowerTokens(it.app);
            addLowerTokens(it.title);
            addLowerTokens(it.rawTitle);
        }
        for (const auto& l : existing) addLowerTokens(l);

        bool grounded = false;
        for (const auto& line : lines)
        {
            std::string cur;
            for (char c : line)
            {
                unsigned char u = (unsigned char)c;
                if (std::isalnum(u))
                    cur.push_back((char)std::tolower(u));
                else
                {
                    if (cur.size() >= 4 && inputTokens.count(cur)) { grounded = true; break; }
                    cur.clear();
                }
            }
            if (grounded) break;
            if (cur.size() >= 4 && inputTokens.count(cur)) grounded = true;
            if (grounded) break;
        }
        if (!grounded) return {};

        return lines;
    }
    catch (...)
    {
        cleanup();
        return {};
    }
#else
    (void)existing; (void)items; (void)category;
    return {};
#endif
}
