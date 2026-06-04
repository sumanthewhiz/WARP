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
        std::ostringstream sys;
        sys << "You summarize a user's current desktop activity. "
               "Produce 1 to 3 short natural-language lines that describe what "
               "the user is doing right now. Be specific and concrete -- name "
               "the documents, websites, or apps actually present in the input. "
               "Do not invent details that are not in the input. "
               "Do not include disclaimers, lists, JSON, or quotes around your "
               "output. Each line should be a complete sentence or sentence "
               "fragment. Aim for natural, descriptive language -- not a list "
               "of bullet points.";

        if (category == "files")
            sys << " Focus only on documents and files the user has open.";
        else if (category == "websites")
            sys << " Focus only on web pages the user is browsing.";
        else if (category == "apps")
            sys << " Focus only on communication and utility apps "
                   "(email, chat, terminals, media, remote desktop).";

        std::ostringstream user;
        user << "User's current activities (most-focused first):\n";
        size_t shown = 0;
        for (const auto& it : items)
        {
            if (shown >= kMaxPromptItems) break;
            user << "- " << it.app << ": \""
                 << Truncate(it.title.empty() ? it.rawTitle : it.title,
                             kMaxPromptTitleLen)
                 << "\" (" << it.pct << "% of focus)\n";
            ++shown;
        }
        if (shown == 0) user << "- (no activity)\n";

        if (!existing.empty())
        {
            user << "\nCandidate summary (refine or replace; one sentence per line):\n";
            for (const auto& line : existing)
                user << line << "\n";
        }
        user << "\nRefined summary (1-3 lines, no numbering, no leading dashes):";

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
            if (!line.empty() && line.size() <= kMaxPolishedLineLen)
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
