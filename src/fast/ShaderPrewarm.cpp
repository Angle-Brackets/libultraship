#include "fast/ShaderPrewarm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include <spdlog/spdlog.h>

namespace Fast::ShaderPrewarm {
namespace {
// Shaders before the first EnterArea (title screen, file select).
constexpr const char* kBootArea = "boot";
// A shader recorded in this many areas is assumed to be UI/common and joins the global set.
constexpr size_t kGlobalAreaThreshold = 3;

using ShaderId = std::pair<uint64_t, uint64_t>;

std::mutex sMutex;
std::atomic<bool> sEnabled { false };
std::map<std::string, std::set<ShaderId>> sByArea; // everything known, file + this session
std::string sCurrentArea = kBootArea;
std::set<ShaderId> sRecordedHere; // render thread: already noted for sCurrentArea
std::string sRecordedArea;
std::ofstream sListOut;

std::vector<ShaderId> sPending;
std::vector<std::string> sPendingLabels;

// PrepareArea: the area being faded towards, and what's left of its list.
std::string sPreparedArea;
std::vector<ShaderId> sTrickle;
std::chrono::steady_clock::time_point sPreparedAt;
int sGraceMs = 0;

// Area links: how often `to` followed `from`. Each traversal is appended until this many.
constexpr uint32_t kMaxLinkLines = 32;
std::map<std::string, std::map<std::string, uint32_t>> sLinks;
std::map<std::string, std::string> sValues;
std::vector<ShaderId> sBackground; // most likely at the back
// Handed out by TakeOne/TakePending, so compiled (the program pool never evicts).
std::set<ShaderId> sHanded;

std::vector<std::string> LinksByCountLocked(const std::string& from) {
    std::vector<std::pair<uint32_t, std::string>> counted;
    if (auto it = sLinks.find(from); it != sLinks.end()) {
        for (const auto& [to, count] : it->second) {
            counted.push_back({ count, to });
        }
    }
    std::sort(counted.rbegin(), counted.rend());
    std::vector<std::string> out;
    for (const auto& [count, to] : counted) {
        out.push_back(to);
    }
    return out;
}

// Background order: the seeds, their links by count, then every other known area.
void BuildBackgroundLocked(const std::vector<std::string>& seeds) {
    std::vector<std::string> order;
    std::set<std::string> seen { sCurrentArea };
    auto add = [&](const std::string& area) {
        if (seen.insert(area).second) {
            order.push_back(area);
        }
    };
    for (const auto& seed : seeds) {
        add(seed);
    }
    for (const auto& seed : seeds) {
        for (const auto& to : LinksByCountLocked(seed)) {
            add(to);
        }
    }
    for (const auto& [area, ids] : sByArea) {
        add(area);
    }
    std::set<ShaderId> queued;
    std::vector<ShaderId> inOrder;
    for (const auto& area : order) {
        if (auto it = sByArea.find(area); it != sByArea.end()) {
            for (const auto& id : it->second) {
                if (!sHanded.count(id) && queued.insert(id).second) {
                    inOrder.push_back(id);
                }
            }
        }
    }
    sBackground.assign(inOrder.rbegin(), inOrder.rend());
}

// Skips shaders already handed out.
bool PopLocked(std::vector<ShaderId>& queue, ShaderId& out) {
    while (!queue.empty()) {
        out = queue.back();
        queue.pop_back();
        if (sHanded.insert(out).second) {
            return true;
        }
    }
    return false;
}

std::set<ShaderId> GlobalSetLocked() {
    std::map<ShaderId, size_t> areaCounts;
    for (const auto& [area, ids] : sByArea) {
        for (const auto& id : ids) {
            areaCounts[id]++;
        }
    }
    std::set<ShaderId> global;
    if (auto boot = sByArea.find(kBootArea); boot != sByArea.end()) {
        global = boot->second;
    }
    for (const auto& [id, count] : areaCounts) {
        if (count >= kGlobalAreaThreshold) {
            global.insert(id);
        }
    }
    return global;
}

void QueueLocked(const std::set<ShaderId>& ids, const std::string& label) {
    sPending.insert(sPending.end(), ids.begin(), ids.end());
    sPendingLabels.push_back(label);
}
} // namespace

void Init(const std::string& listPath) {
    std::lock_guard<std::mutex> lock(sMutex);
    size_t entries = 0;
    {
        std::ifstream in(listPath);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            std::string area;
            // "@ next <from> <to>" / "@ set <key> <value>". Older builds skip these lines: "n"/"s"
            // aren't hex digits, so their "<area> <hex> <hex>" parse fails.
            if (line.rfind("@ ", 0) == 0) {
                std::string at, kind, a, b;
                fields >> at >> kind >> a >> b;
                if (kind == "next" && !b.empty()) {
                    sLinks[a][b]++;
                } else if (kind == "set" && !a.empty()) {
                    sValues[a] = b;
                }
                continue;
            }
            uint64_t id0 = 0, id1 = 0;
            if (fields >> area >> std::hex >> id0 >> id1) {
                sByArea[area].insert({ id0, id1 });
                entries++;
            }
        }
    }
    sListOut.open(listPath, std::ios::app);
    sEnabled = true;
    SPDLOG_INFO("[shaders] prewarm list: {} entries across {} areas, {} global", entries, sByArea.size(),
                GlobalSetLocked().size());
}

bool Enabled() {
    return sEnabled;
}

void EnterArea(const std::string& area) {
    if (!sEnabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    if (area == sCurrentArea) {
        return; // a room change inside the area: its shaders were compiled on the way in
    }
    sCurrentArea = area;
    if (area == sPreparedArea) {
        // Whatever the fade didn't get to: compiled in the load's idle time or the first frame.
        sPending.insert(sPending.end(), sTrickle.begin(), sTrickle.end());
        sPendingLabels.push_back(area);
        sTrickle.clear();
    } else if (auto it = sByArea.find(area); it != sByArea.end()) {
        QueueLocked(it->second, area);
    }
    sPreparedArea.clear();
    // Battles have no links of their own: keep the world area's background through them.
    if (sLinks.count(area) || sBackground.empty()) {
        BuildBackgroundLocked(LinksByCountLocked(area));
    }
}

void NoteLink(const std::string& from, const std::string& to) {
    if (!sEnabled || from.empty() || to.empty() || from == to) {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    if (++sLinks[from][to] <= kMaxLinkLines && sListOut) {
        sListOut << "@ next " << from << ' ' << to << '\n';
        sListOut.flush();
    }
}

void Hint(const std::vector<std::string>& areas) {
    if (!sEnabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    BuildBackgroundLocked(areas);
}

void Remember(const std::string& key, const std::string& value) {
    if (!sEnabled || key.empty() || value.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    if (sValues[key] == value) {
        return;
    }
    sValues[key] = value;
    if (sListOut) {
        sListOut << "@ set " << key << ' ' << value << '\n';
        sListOut.flush();
    }
}

std::string Recall(const std::string& key) {
    std::lock_guard<std::mutex> lock(sMutex);
    auto it = sValues.find(key);
    return it == sValues.end() ? std::string() : it->second;
}

void PrepareArea(const std::string& area, int graceMs) {
    if (!sEnabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    if (area == sCurrentArea || area == sPreparedArea) {
        return;
    }
    sPreparedArea = area;
    sGraceMs = graceMs;
    sPreparedAt = std::chrono::steady_clock::now();
    sTrickle.clear();
    if (auto it = sByArea.find(area); it != sByArea.end()) {
        sTrickle.assign(it->second.begin(), it->second.end());
    }
}

bool TakeOne(std::pair<uint64_t, uint64_t>& out, bool drawing, bool allowBackground) {
    if (!sEnabled) {
        return false;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    if (drawing) {
        if (!sTrickle.empty()) {
            if (std::chrono::steady_clock::now() - sPreparedAt < std::chrono::milliseconds(sGraceMs)) {
                return false;
            }
            if (PopLocked(sTrickle, out)) {
                return true;
            }
        }
        return allowBackground && PopLocked(sBackground, out);
    }
    return PopLocked(sTrickle, out) || PopLocked(sPending, out) || (allowBackground && PopLocked(sBackground, out));
}

void RequestGlobal() {
    if (!sEnabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    QueueLocked(GlobalSetLocked(), "global");
}

void NoteUse(uint64_t id0, uint64_t id1) {
    if (!sEnabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    if (sRecordedArea != sCurrentArea) {
        sRecordedArea = sCurrentArea;
        sRecordedHere.clear();
    }
    const ShaderId id { id0, id1 };
    if (!sRecordedHere.insert(id).second) {
        return;
    }
    if (sByArea[sCurrentArea].insert(id).second && sListOut) {
        // New for this area: persist it. Rare after the first visit, so flushing each line is fine.
        sListOut << sCurrentArea << ' ' << std::hex << id0 << ' ' << id1 << std::dec << '\n';
        sListOut.flush();
    }
}

bool Busy() {
    if (!sEnabled) {
        return false;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    return !sTrickle.empty() || !sPending.empty();
}

bool TakePending(std::vector<std::pair<uint64_t, uint64_t>>& out, std::string& label) {
    if (!sEnabled) {
        return false;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    if (sPendingLabels.empty()) {
        return false;
    }
    out.swap(sPending);
    sPending.clear();
    sHanded.insert(out.begin(), out.end());
    label.clear();
    for (const auto& l : sPendingLabels) {
        label += label.empty() ? l : "+" + l;
    }
    sPendingLabels.clear();
    return true;
}

} // namespace Fast::ShaderPrewarm
