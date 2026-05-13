#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cctype>

// Minimal BERT WordPiece tokenizer.  Compatible with both
// `BAAI/bge-small-en-v1.5` (the current default model) and the legacy
// `sentence-transformers/all-MiniLM-L6-v2` -- both share the standard
// `bert-base-uncased` 30 522-entry WordPiece vocabulary.
// Loads vocab.txt once, then tokenizes text into token IDs
// with [CLS] / [SEP] framing.
class BertTokenizer
{
public:
    bool Load(const std::string& vocabPath)
    {
        std::ifstream f(vocabPath);
        if (!f.is_open()) return false;

        std::string line;
        int id = 0;
        while (std::getline(f, line))
        {
            // Remove trailing \r if present
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            m_vocab[line] = id;
            if (id < 200)                   // cache special tokens
            {
                if (line == "[CLS]")  m_clsId = id;
                if (line == "[SEP]")  m_sepId = id;
                if (line == "[UNK]")  m_unkId = id;
                if (line == "[PAD]")  m_padId = id;
            }
            ++id;
        }
        m_loaded = true;
        return !m_vocab.empty();
    }

    bool IsLoaded() const { return m_loaded; }

    // Tokenize text -> token IDs with [CLS] ... [SEP] framing.
    // maxLen includes the two special tokens.
    std::vector<int64_t> Encode(const std::string& text, int maxLen = 128) const
    {
        std::vector<int64_t> ids;
        ids.push_back(m_clsId);

        // Basic pre-tokenization: lowercase + split on whitespace/punct
        std::vector<std::string> words = BasicTokenize(text);

        for (const auto& word : words)
        {
            WordPiece(word, ids, maxLen - 1); // -1 for final [SEP]
            if ((int)ids.size() >= maxLen - 1)
                break;
        }

        ids.push_back(m_sepId);

        // Truncate
        if ((int)ids.size() > maxLen)
            ids.resize(maxLen);

        return ids;
    }

    int PadId()  const { return m_padId; }
    int ClsId()  const { return m_clsId; }
    int SepId()  const { return m_sepId; }

private:
    std::unordered_map<std::string, int> m_vocab;
    int  m_clsId = 101;
    int  m_sepId = 102;
    int  m_unkId = 100;
    int  m_padId = 0;
    bool m_loaded = false;

    // Split on whitespace and punctuation, lowercase everything.
    static std::vector<std::string> BasicTokenize(const std::string& text)
    {
        std::vector<std::string> tokens;
        std::string current;

        for (size_t i = 0; i < text.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(text[i]);
            char lc = static_cast<char>(std::tolower(c));

            if (std::isspace(c))
            {
                if (!current.empty()) { tokens.push_back(current); current.clear(); }
            }
            else if (std::ispunct(c))
            {
                if (!current.empty()) { tokens.push_back(current); current.clear(); }
                tokens.push_back(std::string(1, lc));
            }
            else
            {
                current += lc;
            }
        }
        if (!current.empty()) tokens.push_back(current);
        return tokens;
    }

    // WordPiece sub-word tokenization of a single pre-tokenized word.
    void WordPiece(const std::string& word, std::vector<int64_t>& ids,
                   int maxLen) const
    {
        if (word.empty()) return;

        // Try to tokenize greedily left-to-right
        size_t start = 0;
        bool isFirst = true;

        while (start < word.size())
        {
            if ((int)ids.size() >= maxLen) return;

            std::string bestSub;
            int bestId = m_unkId;
            size_t bestEnd = start + 1;

            // Try longest match first
            for (size_t end = word.size(); end > start; --end)
            {
                std::string sub = word.substr(start, end - start);
                if (!isFirst)
                    sub = "##" + sub;

                auto it = m_vocab.find(sub);
                if (it != m_vocab.end())
                {
                    bestSub = sub;
                    bestId = it->second;
                    bestEnd = end;
                    break;
                }
            }

            ids.push_back(bestId);

            if (bestId == m_unkId && bestSub.empty())
            {
                // Character not found at all, skip it
                start++;
            }
            else
            {
                start = bestEnd;
            }
            isFirst = false;
        }
    }
};
