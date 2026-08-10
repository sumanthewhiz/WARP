#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <cstdint>
#include <climits>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// ============================================================================
//  ModernBertTokenizer
// ============================================================================
//  Hand-rolled byte-level BPE tokenizer compatible with the
//  HuggingFace "fast tokenizers" pipeline used by ModernBERT-family
//  embedding models -- specifically `ibm-granite/granite-embedding-
//  small-english-r2`, where the JSON tokenizer config declares:
//
//      model.type        = "BPE"
//      pre_tokenizer     = ByteLevel (use_regex=true, add_prefix_space=false)
//      normalizer        = NFC
//      post_processor    = TemplateProcessing([CLS] A [SEP])
//      decoder           = ByteLevel
//
//  This header consumes the flat artefacts emitted by the build-time
//  `scripts/extract_modernbert_tokenizer.py` script:
//
//      <modelsDir>/granite/vocab.txt           one byte-level-encoded token
//                                              per line, line N (0-indexed)
//                                              = token ID N
//      <modelsDir>/granite/merges.txt          one "a b" pair per line, in
//                                              HF rank order
//      <modelsDir>/granite/special_tokens.txt  key=id pairs for
//                                              [CLS] / [SEP] / [PAD] /
//                                              [UNK] / [MASK]
//
//  The reference implementation lives at
//  `scripts/modernbert_tokenizer_ref.py` and is verified bit-exact
//  against `transformers.AutoTokenizer` for 11 representative window-
//  title strings (code identifiers, contractions, NFC accents, paths,
//  version numbers).  Algorithm changes here MUST keep the python
//  reference and this C++ port in lockstep.
// ============================================================================
class ModernBertTokenizer
{
public:
    bool Load(const std::string& vocabPath,
              const std::string& mergesPath,
              const std::string& specialPath)
    {
        BuildByteMap();

        // ---- vocab.txt --------------------------------------------------
        {
            std::ifstream f(vocabPath);
            if (!f.is_open()) return false;
            std::string line;
            int id = 0;
            while (std::getline(f, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                m_vocab.emplace(std::move(line), id++);
            }
            if (m_vocab.empty()) return false;
        }

        // ---- merges.txt -------------------------------------------------
        {
            std::ifstream f(mergesPath);
            if (!f.is_open()) return false;
            std::string line;
            int rank = 0;
            while (std::getline(f, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                m_mergeRank.emplace(std::move(line), rank++);
            }
            if (m_mergeRank.empty()) return false;
        }

        // ---- special_tokens.txt ----------------------------------------
        {
            std::ifstream f(specialPath);
            if (!f.is_open()) return false;
            std::string line;
            while (std::getline(f, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                int val = std::atoi(line.c_str() + eq + 1);
                if      (key == "cls_id")  m_clsId  = val;
                else if (key == "sep_id")  m_sepId  = val;
                else if (key == "pad_id")  m_padId  = val;
                else if (key == "unk_id")  m_unkId  = val;
                else if (key == "mask_id") m_maskId = val;
            }
            if (m_clsId < 0 || m_sepId < 0 || m_padId < 0 || m_unkId < 0)
                return false;
        }

        m_loaded = true;
        return true;
    }

    bool IsLoaded() const { return m_loaded; }
    int  PadId()    const { return m_padId; }
    int  ClsId()    const { return m_clsId; }
    int  SepId()    const { return m_sepId; }

    // Tokenize text -> token IDs with [CLS] ... [SEP] framing.  maxLen
    // includes the two special tokens.  Mirrors the Python reference
    // `ModernBertTokenizer.encode` exactly.
    std::vector<int64_t> Encode(const std::string& text,
                                int maxLen = 128) const
    {
        std::vector<int64_t> ids;
        ids.reserve(static_cast<size_t>(maxLen));
        ids.push_back(m_clsId);

        std::string normalized = NormalizeNFC(text);
        std::vector<std::string> preTokens = PreTokenize(normalized);

        const int hardCap = maxLen - 1; // reserve slot for [SEP]
        for (const std::string& pt : preTokens)
        {
            std::string encoded = ByteLevelEncode(pt);
            ApplyBpe(encoded, ids, hardCap);
            if (static_cast<int>(ids.size()) >= hardCap) break;
        }

        if (static_cast<int>(ids.size()) > hardCap) ids.resize(hardCap);
        ids.push_back(m_sepId);
        return ids;
    }

private:
    // ------------------------------------------------------------------
    //  GPT-2 byte -> visible-Unicode mapping (256-entry table).
    //  Bytes 0x21-0x7E, 0xA1-0xAC, 0xAE-0xFF map to themselves.
    //  The remaining 68 bytes map to codepoints 0x100, 0x101, ...
    //  in the order they are encountered while scanning 0..255.
    // ------------------------------------------------------------------
    void BuildByteMap()
    {
        if (!m_byteMap[0].empty()) return; // already built

        std::vector<int> bs;
        bs.reserve(256);
        for (int b = 0x21; b <= 0x7E; ++b) bs.push_back(b);
        for (int b = 0xA1; b <= 0xAC; ++b) bs.push_back(b);
        for (int b = 0xAE; b <= 0xFF; ++b) bs.push_back(b);

        std::vector<int> cs = bs;
        int extra = 0;
        std::vector<char> inSet(256, 0);
        for (int b : bs) inSet[b] = 1;
        for (int b = 0; b < 256; ++b)
        {
            if (!inSet[b])
            {
                bs.push_back(b);
                cs.push_back(256 + extra);
                ++extra;
            }
        }
        for (size_t i = 0; i < bs.size(); ++i)
            m_byteMap[bs[i]] = CodepointToUtf8(cs[i]);
    }

    static std::string CodepointToUtf8(int cp)
    {
        std::string s;
        if (cp < 0x80)
        {
            s.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            s.push_back(static_cast<char>(0xE0 |  (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 |  (cp & 0x3F)));
        }
        else
        {
            s.push_back(static_cast<char>(0xF0 |  (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6)  & 0x3F)));
            s.push_back(static_cast<char>(0x80 |  (cp & 0x3F)));
        }
        return s;
    }

    // Apply the 256-entry byte map to every byte of `text`.
    std::string ByteLevelEncode(const std::string& text) const
    {
        std::string out;
        out.reserve(text.size() * 2);
        for (unsigned char b : text) out.append(m_byteMap[b]);
        return out;
    }

    // ------------------------------------------------------------------
    //  Unicode NFC normalization via Windows API.  Returns the input
    //  unchanged if normalization fails (best-effort: avoids breaking
    //  the pipeline on rare malformed UTF-8 sequences).
    // ------------------------------------------------------------------
    static std::string NormalizeNFC(const std::string& utf8)
    {
        if (utf8.empty()) return utf8;

        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                       static_cast<int>(utf8.size()),
                                       nullptr, 0);
        if (wlen <= 0) return utf8;
        std::wstring wide(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                            static_cast<int>(utf8.size()),
                            &wide[0], wlen);

        int normLen = NormalizeString(NormalizationC, wide.c_str(), wlen,
                                      nullptr, 0);
        if (normLen <= 0) return utf8;
        std::wstring norm(static_cast<size_t>(normLen), L'\0');
        int actual = NormalizeString(NormalizationC, wide.c_str(), wlen,
                                     &norm[0], normLen);
        if (actual <= 0) return utf8;
        norm.resize(static_cast<size_t>(actual));

        int outLen = WideCharToMultiByte(CP_UTF8, 0, norm.c_str(), actual,
                                         nullptr, 0, nullptr, nullptr);
        if (outLen <= 0) return utf8;
        std::string out(static_cast<size_t>(outLen), '\0');
        WideCharToMultiByte(CP_UTF8, 0, norm.c_str(), actual,
                            &out[0], outLen, nullptr, nullptr);
        return out;
    }

    // ------------------------------------------------------------------
    //  UTF-8 codepoint walker.
    // ------------------------------------------------------------------
    static int DecodeUtf8(const char* p, const char* end, int& outBytes)
    {
        unsigned char b0 = static_cast<unsigned char>(*p);
        if (b0 < 0x80) { outBytes = 1; return b0; }
        if ((b0 & 0xE0) == 0xC0 && p + 1 < end)
        {
            outBytes = 2;
            return ((b0 & 0x1F) << 6)
                 | (static_cast<unsigned char>(p[1]) & 0x3F);
        }
        if ((b0 & 0xF0) == 0xE0 && p + 2 < end)
        {
            outBytes = 3;
            return ((b0 & 0x0F) << 12)
                 | ((static_cast<unsigned char>(p[1]) & 0x3F) << 6)
                 |  (static_cast<unsigned char>(p[2]) & 0x3F);
        }
        if ((b0 & 0xF8) == 0xF0 && p + 3 < end)
        {
            outBytes = 4;
            return ((b0 & 0x07) << 18)
                 | ((static_cast<unsigned char>(p[1]) & 0x3F) << 12)
                 | ((static_cast<unsigned char>(p[2]) & 0x3F) << 6)
                 |  (static_cast<unsigned char>(p[3]) & 0x3F);
        }
        // Malformed -- consume one byte to make progress.
        outBytes = 1;
        return b0;
    }

    // ------------------------------------------------------------------
    //  Codepoint classifiers matching the GPT-2 pre-tokenizer regex
    //  `'(?:[sdmt]|ll|ve|re)| ?\p{L}+| ?\p{N}+|
    //   ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+`.
    //
    //  For non-ASCII codepoints we use a coarse but sound rule:
    //  Cf / Cc control characters and U+0085, U+00A0, U+2028, U+2029
    //  count as whitespace; CJK / Arabic / Cyrillic / Latin-extended /
    //  Greek / Hebrew letters are recognized via Unicode block ranges;
    //  digits via a small union of decimal-digit blocks; everything
    //  else falls into the "other" bucket -- consistent with how the
    //  HF regex would treat them for window-title clustering input.
    // ------------------------------------------------------------------
    enum CharClass { CLS_LETTER, CLS_DIGIT, CLS_SPACE, CLS_OTHER };

    static CharClass Classify(int cp)
    {
        if (cp < 0x80)
        {
            if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
                cp == '\f' || cp == '\v')
                return CLS_SPACE;
            if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))
                return CLS_LETTER;
            if (cp >= '0' && cp <= '9') return CLS_DIGIT;
            return CLS_OTHER;
        }
        // Common Unicode whitespace
        if (cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
            (cp >= 0x2000 && cp <= 0x200A) ||
            cp == 0x2028 || cp == 0x2029 ||
            cp == 0x202F || cp == 0x205F || cp == 0x3000)
            return CLS_SPACE;
        // Decimal digits (Arabic-Indic, Devanagari, etc. -- common blocks)
        if ((cp >= 0x0660 && cp <= 0x0669) || // Arabic
            (cp >= 0x06F0 && cp <= 0x06F9) || // Extended Arabic
            (cp >= 0x0966 && cp <= 0x096F) || // Devanagari
            (cp >= 0xFF10 && cp <= 0xFF19))   // Fullwidth
            return CLS_DIGIT;
        // Latin-1 letter ranges + common Latin extended
        if ((cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7) ||
            (cp >= 0x100 && cp <= 0x2AF) ||
            (cp >= 0x370 && cp <= 0x3FF && cp != 0x37E) || // Greek
            (cp >= 0x400 && cp <= 0x4FF) ||                // Cyrillic
            (cp >= 0x500 && cp <= 0x52F) ||                // Cyrillic Supp.
            (cp >= 0x530 && cp <= 0x58F) ||                // Armenian
            (cp >= 0x590 && cp <= 0x5FF) ||                // Hebrew
            (cp >= 0x600 && cp <= 0x6FF) ||                // Arabic
            (cp >= 0x900 && cp <= 0x97F) ||                // Devanagari
            (cp >= 0xA00 && cp <= 0xA7F) ||                // Gurmukhi
            (cp >= 0xA80 && cp <= 0xAFF) ||                // Gujarati
            (cp >= 0xB00 && cp <= 0xBFF) ||                // Tamil
            (cp >= 0xC00 && cp <= 0xCFF) ||                // Telugu/Kannada
            (cp >= 0xD00 && cp <= 0xD7F) ||                // Malayalam
            (cp >= 0xE00 && cp <= 0xEFF) ||                // Thai/Lao
            (cp >= 0x1100 && cp <= 0x11FF) ||              // Hangul Jamo
            (cp >= 0x3040 && cp <= 0x309F) ||              // Hiragana
            (cp >= 0x30A0 && cp <= 0x30FF) ||              // Katakana
            (cp >= 0x3400 && cp <= 0x4DBF) ||              // CJK Ext A
            (cp >= 0x4E00 && cp <= 0x9FFF) ||              // CJK
            (cp >= 0xAC00 && cp <= 0xD7AF) ||              // Hangul
            (cp >= 0xF900 && cp <= 0xFAFF))                // CJK Compat
            return CLS_LETTER;
        return CLS_OTHER;
    }

    // ------------------------------------------------------------------
    //  Pre-tokenize per the GPT-2 regex, simplified for the practical
    //  input WARP handles (English window titles with occasional code
    //  identifiers, paths, version numbers, and non-ASCII names).
    //
    //  Emits pre-tokens in this order:
    //    - English contractions  'sdmt|ll|ve|re        (e.g. "Don't")
    //    - " ?<LETTER>+"
    //    - " ?<DIGIT>+"
    //    - " ?<OTHER>+"
    //    - whitespace runs with the trailing space stripped off when
    //      followed by a non-space character (matches `\s+(?!\S)`
    //      then `\s+`)
    // ------------------------------------------------------------------
    static std::vector<std::string> PreTokenize(const std::string& text)
    {
        std::vector<std::string> out;
        const char* p   = text.c_str();
        const char* end = p + text.size();

        while (p < end)
        {
            // ---- contractions: '(?:[sdmt]|ll|ve|re) -----------------
            if (*p == '\'' && p + 1 < end)
            {
                char c1 = p[1];
                if (c1 == 's' || c1 == 'd' || c1 == 'm' || c1 == 't')
                {
                    out.emplace_back(p, 2);
                    p += 2;
                    continue;
                }
                if (p + 2 < end &&
                    ((c1 == 'l' && p[2] == 'l') ||
                     (c1 == 'v' && p[2] == 'e') ||
                     (c1 == 'r' && p[2] == 'e')))
                {
                    out.emplace_back(p, 3);
                    p += 3;
                    continue;
                }
            }

            int firstBytes = 0;
            int firstCp = DecodeUtf8(p, end, firstBytes);
            CharClass firstClass = Classify(firstCp);

            // ---- whitespace handling --------------------------------
            if (firstClass == CLS_SPACE)
            {
                // Consume the entire run of whitespace.
                const char* runStart = p;
                while (p < end)
                {
                    int wsBytes = 0;
                    int wsCp = DecodeUtf8(p, end, wsBytes);
                    if (Classify(wsCp) != CLS_SPACE) break;
                    p += wsBytes;
                }
                size_t runLen = static_cast<size_t>(p - runStart);
                if (p >= end)
                {
                    // Trailing whitespace -- whole run is a pre-token.
                    out.emplace_back(runStart, runLen);
                }
                else
                {
                    // \s+(?!\S) consumes all but the last whitespace
                    // char; the remaining single space is then folded
                    // into the next " ?<class>+" run.  When the run is
                    // exactly one space the (?!\S) variant matches
                    // nothing and the lone space becomes the prefix.
                    // Find the LAST whitespace codepoint in the run.
                    const char* lastWs = nullptr;
                    {
                        const char* q = runStart;
                        while (q < p)
                        {
                            int qb = 0;
                            DecodeUtf8(q, p, qb);
                            lastWs = q;
                            q += qb;
                        }
                    }
                    if (lastWs && lastWs > runStart)
                    {
                        // Emit prefix run (everything except last ws cp).
                        out.emplace_back(runStart,
                                         static_cast<size_t>(lastWs - runStart));
                    }
                    // Re-classify what follows the last whitespace cp.
                    int nextBytes = 0;
                    int nextCp = DecodeUtf8(lastWs ? lastWs : runStart,
                                            end, nextBytes);
                    (void)nextCp;
                    int afterBytes = 0;
                    int afterCp = DecodeUtf8(p, end, afterBytes);
                    CharClass afterClass = Classify(afterCp);

                    // " ?<class>+" -- include the single leading ws char.
                    const char* tokStart =
                        lastWs ? lastWs : runStart;
                    const char* q = p;
                    while (q < end)
                    {
                        int qb = 0;
                        int qcp = DecodeUtf8(q, end, qb);
                        if (Classify(qcp) != afterClass) break;
                        q += qb;
                    }
                    out.emplace_back(tokStart,
                                     static_cast<size_t>(q - tokStart));
                    p = q;
                }
                continue;
            }

            // ---- letter / digit / other run (no leading space) -----
            const char* tokStart = p;
            p += firstBytes;
            while (p < end)
            {
                int nb = 0;
                int ncp = DecodeUtf8(p, end, nb);
                if (Classify(ncp) != firstClass) break;
                p += nb;
            }
            out.emplace_back(tokStart, static_cast<size_t>(p - tokStart));
        }
        return out;
    }

    // ------------------------------------------------------------------
    //  Apply BPE to a single byte-level-encoded pre-token.
    //
    //  Algorithm (matches the python reference exactly):
    //    1. Fast path: if the whole pre-token is already in the vocab,
    //       emit that single ID (this is the `ignore_merges`-style
    //       lookup used by HF when the entry exists verbatim).
    //    2. Split into codepoint-sized pieces.
    //    3. Repeatedly find the lowest-rank merge among all adjacent
    //       pairs and apply it, until no more merges apply.
    //    4. Emit one ID per remaining piece (UNK fallback if missing).
    // ------------------------------------------------------------------
    void ApplyBpe(const std::string& token,
                  std::vector<int64_t>& outIds,
                  int hardCap) const
    {
        if (token.empty()) return;
        if (static_cast<int>(outIds.size()) >= hardCap) return;

        auto direct = m_vocab.find(token);
        if (direct != m_vocab.end())
        {
            outIds.push_back(direct->second);
            return;
        }

        std::vector<std::string> parts;
        parts.reserve(token.size());
        {
            const char* p   = token.c_str();
            const char* end = p + token.size();
            while (p < end)
            {
                int nb = 0;
                DecodeUtf8(p, end, nb);
                parts.emplace_back(p, static_cast<size_t>(nb));
                p += nb;
            }
        }

        if (parts.size() < 2)
        {
            auto it = m_vocab.find(token);
            outIds.push_back(it != m_vocab.end() ? it->second : m_unkId);
            return;
        }

        // Iteratively merge the lowest-rank pair.
        // O(n^2) per pre-token, but pre-tokens are short (~20 chars).
        while (parts.size() >= 2)
        {
            int bestRank = INT_MAX;
            int bestIdx  = -1;
            std::string scratch;
            scratch.reserve(32);
            for (size_t i = 0; i + 1 < parts.size(); ++i)
            {
                scratch = parts[i];
                scratch.push_back(' ');
                scratch += parts[i + 1];
                auto it = m_mergeRank.find(scratch);
                if (it != m_mergeRank.end() && it->second < bestRank)
                {
                    bestRank = it->second;
                    bestIdx  = static_cast<int>(i);
                }
            }
            if (bestIdx < 0) break;
            parts[bestIdx] += parts[bestIdx + 1];
            parts.erase(parts.begin() + bestIdx + 1);
        }

        for (const auto& p : parts)
        {
            if (static_cast<int>(outIds.size()) >= hardCap) return;
            auto it = m_vocab.find(p);
            outIds.push_back(it != m_vocab.end() ? it->second : m_unkId);
        }
    }

    // ---- State -------------------------------------------------------
    std::unordered_map<std::string, int> m_vocab;
    std::unordered_map<std::string, int> m_mergeRank;
    std::string m_byteMap[256];
    int  m_clsId  = -1;
    int  m_sepId  = -1;
    int  m_padId  = -1;
    int  m_unkId  = -1;
    int  m_maskId = -1;
    bool m_loaded = false;
};
