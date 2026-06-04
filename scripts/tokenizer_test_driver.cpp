// Standalone test driver for ModernBertTokenizer.
// Compares C++ tokenization to a JSON file containing reference IDs from
// the Python `transformers.AutoTokenizer` invocation.
//
// Usage:
//   tokenizer_test.exe <modelsDir> <tests.txt> <expected.txt>
//
// tests.txt    one input string per line (UTF-8, LF)
// expected.txt one space-separated ID list per line, in the same order
//
// Exit code: 0 on full match, 1 on any mismatch.

#include "ModernBertTokenizer.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

static std::vector<std::string> ReadLines(const std::string& path)
{
    std::vector<std::string> out;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

static std::vector<int64_t> ParseIds(const std::string& line)
{
    std::vector<int64_t> ids;
    std::istringstream is(line);
    int64_t id;
    while (is >> id) ids.push_back(id);
    return ids;
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "usage: " << argv[0]
                  << " <modelsDir> <tests.txt> <expected.txt>\n";
        return 2;
    }

    ModernBertTokenizer tok;
    std::string dir = argv[1];
    if (!tok.Load(dir + "\\vocab.txt",
                  dir + "\\merges.txt",
                  dir + "\\special_tokens.txt"))
    {
        std::cerr << "failed to load tokenizer from " << dir << "\n";
        return 3;
    }

    auto tests    = ReadLines(argv[2]);
    auto expected = ReadLines(argv[3]);
    if (tests.size() != expected.size())
    {
        std::cerr << "mismatched lengths\n";
        return 4;
    }

    int fail = 0;
    for (size_t i = 0; i < tests.size(); ++i)
    {
        auto got = tok.Encode(tests[i], 512);
        auto ref = ParseIds(expected[i]);
        bool ok = got.size() == ref.size();
        if (ok)
            for (size_t k = 0; k < got.size(); ++k)
                if (got[k] != ref[k]) { ok = false; break; }
        std::cout << (ok ? "OK  " : "FAIL") << "  '"
                  << tests[i] << "'\n";
        if (!ok)
        {
            ++fail;
            std::cout << "      ref:";
            for (auto x : ref) std::cout << " " << x;
            std::cout << "\n      got:";
            for (auto x : got) std::cout << " " << x;
            std::cout << "\n";
        }
    }
    std::cout << "\n" << (tests.size() - fail) << "/" << tests.size()
              << " matched\n";
    return fail == 0 ? 0 : 1;
}
