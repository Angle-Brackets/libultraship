#include "fast/ShaderPrewarm.h"

#include <atomic>
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
    if (auto it = sByArea.find(area); it != sByArea.end()) {
        QueueLocked(it->second, area);
    }
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
    label.clear();
    for (const auto& l : sPendingLabels) {
        label += label.empty() ? l : "+" + l;
    }
    sPendingLabels.clear();
    return true;
}

} // namespace Fast::ShaderPrewarm
