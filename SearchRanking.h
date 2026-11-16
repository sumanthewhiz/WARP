#pragma once
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace WarpSearch
{
    struct RankInput { double base = 0, delta = 0; bool exact = false; };

    inline double BoundedDelta(const RankInput& item)
    {
        return std::isfinite(item.base) && item.base >= 20 && std::isfinite(item.delta) ?
            (std::max)(0.0, (std::min)(8.0, item.delta)) : 0.0;
    }

    inline std::vector<size_t> Rank(const std::vector<RankInput>& input, bool contextual)
    {
        contextual = contextual && std::any_of(input.begin(), input.end(),
            [](const RankInput& item) { return BoundedDelta(item) > 0; });
        std::vector<size_t> order(input.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (contextual && input[a].exact != input[b].exact) return input[a].exact;
            auto score = [&](size_t i) {
                const double base = std::isfinite(input[i].base) ? input[i].base : 0;
                return base + (contextual ? BoundedDelta(input[i]) : 0);
            };
            return score(a) > score(b);
        });
        return order;
    }
}
