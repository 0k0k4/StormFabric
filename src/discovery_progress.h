#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace swml {
namespace discovery_progress {

inline std::string SafeText(const char* text, size_t limit) {
    std::string result;
    if (!text) return result;
    for (; *text && result.size() < limit; ++text) {
        unsigned char c = static_cast<unsigned char>(*text);
        result += (c >= 32 && c <= 126) ? static_cast<char>(c) : ' ';
    }
    return result;
}

struct State {
    bool valid = false;
    bool finished = false;
    bool failed = false;
    size_t completed = 0;
    size_t total = 0;
    unsigned percent = 0;
    std::string phase;
    std::string detail;

    // Returns true only for useful persistent checkpoints, not each scanned byte.
    bool Update(const char* nextPhase, size_t nextCompleted, size_t nextTotal,
                const char* nextDetail, bool nextFailed) {
        std::string p = SafeText(nextPhase, 160);
        std::string d = SafeText(nextDetail, 240);
        bool done = nextTotal > 0 && nextCompleted >= nextTotal;
        unsigned pc = done ? 100 : nextTotal == 0 ? 0 : (std::min)(99u,
            static_cast<unsigned>(100.0L * nextCompleted / nextTotal));
        bool checkpoint = !valid || phase != p || detail != d || failed != nextFailed ||
            total != nextTotal || finished != done || pc / 10 != percent / 10 ||
            nextCompleted < completed;
        valid = true;
        phase = std::move(p);
        detail = std::move(d);
        completed = nextCompleted;
        total = nextTotal;
        finished = done;
        failed = nextFailed;
        percent = pc;
        return checkpoint;
    }

    std::string Line() const {
        size_t filled = percent / 5;
        std::string line = "[Discovery] [" + std::string(filled, '#') +
            std::string(20 - filled, '-') + "] " +
            (total ? std::to_string(percent) + "%" : "--%") + " " +
            std::to_string(completed) + "/" + (total ? std::to_string(total) : "?") +
            " " + (failed ? "FAILED" : finished ? "PHASE FINISHED" : "RUNNING") + " | " + phase;
        if (!detail.empty()) line += " | " + detail;
        if (finished) line += " (work finished; compatibility not implied)";
        return line;
    }
};

}  // namespace discovery_progress
}  // namespace swml
