#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

namespace Attention
{
    // Missing measurement is -1, not measured zero. Clipping uses cumulative floors so
    // adjacent query windows conserve each measured total, even for long mixed segments.
    inline int Clip(int seconds, int64_t start, int64_t end, int64_t from, int64_t to)
    {
        if (seconds < 0) return -1;
        if (end <= start) return 0;
        const int64_t a = (std::max)(start, from), b = (std::min)(end, to);
        if (b <= a) return 0;
        const double rate = (std::min)(1.0, static_cast<double>(seconds) / (end - start));
        return static_cast<int>(std::floor((b - start) * rate + 1e-9) -
                                std::floor((a - start) * rate + 1e-9));
    }

    struct Span
    {
        int64_t start = 0, end = 0;
        double active = 0, passive = 0; // fractions of covered wall time, not additive sensor votes
    };

    inline std::vector<Span> Union(const std::vector<Span>& input)
    {
        struct Edge { int64_t time; double active, passive; bool begin; };
        std::vector<Edge> edges;
        for (const auto& s : input)
        {
            if (s.end <= s.start) continue;
            const double a = (std::max)(0.0, (std::min)(1.0, s.active));
            const double p = (std::max)(0.0, (std::min)(1.0 - a, s.passive));
            edges.push_back({ s.start, a, p, true });
            edges.push_back({ s.end, a, p, false });
        }
        std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.time < b.time; });
        std::multiset<double> active, passive;
        std::vector<Span> out;
        int64_t previous = edges.empty() ? 0 : edges.front().time;
        for (size_t i = 0; i < edges.size();)
        {
            const int64_t now = edges[i].time;
            if (now > previous && !active.empty())
            {
                const double a = *active.rbegin(), p = (std::min)(1.0 - a, *passive.rbegin());
                if (!out.empty() && out.back().end == previous && out.back().active == a && out.back().passive == p)
                    out.back().end = now;
                else
                    out.push_back({ previous, now, a, p });
            }
            do
            {
                const auto& e = edges[i++];
                if (e.begin) { active.insert(e.active); passive.insert(e.passive); }
                else
                {
                    active.erase(active.find(e.active));
                    passive.erase(passive.find(e.passive));
                }
            } while (i < edges.size() && edges[i].time == now);
            previous = now;
        }
        return out;
    }
}
