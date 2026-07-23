#pragma once
#include "IMemoryAllocator.h"
#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <algorithm>

// Represents one allocated block in memory.
struct MemBlock {
    uint32_t start;      // inclusive byte address
    uint32_t end;        // inclusive byte address (start + memPerProc - 1)
    std::string procName;
};

class MemoryAllocator : public IMemoryAllocator {
private:
    uint32_t totalMem;
    uint32_t memPerProc;
    uint32_t memPerFrame;
    std::vector<MemBlock> blocks; // sorted by start address
    std::mutex mtx;

public:
    MemoryAllocator(uint32_t totalMem, uint32_t memPerProc, uint32_t memPerFrame)
        : totalMem(totalMem), memPerProc(memPerProc), memPerFrame(memPerFrame) {
    }

    // Attempts to allocate memPerProc bytes using first-fit.
    // Returns true and sets startAddr on success; returns false if memory is full.
    bool allocate(const std::string& procName, uint32_t& startAddr) override {
        std::lock_guard<std::mutex> lock(mtx);

        // Build candidate start positions: 0, then right after each existing block.
        std::vector<uint32_t> candidates;
        candidates.push_back(0);
        for (auto& b : blocks)
            candidates.push_back(b.end + 1);

        // Sort blocks by start for first-fit scanning.
        std::sort(blocks.begin(), blocks.end(),
            [](const MemBlock& a, const MemBlock& b) { return a.start < b.start; });

        for (uint32_t candidate : candidates) {
            uint32_t end = candidate + memPerProc - 1;
            if (end >= totalMem) break; // won't fit

            // Check candidate range [candidate, end] doesn't overlap any block.
            bool fits = true;
            for (auto& b : blocks) {
                // Overlap if not (end < b.start || candidate > b.end)
                if (!(end < b.start || candidate > b.end)) {
                    fits = false;
                    break;
                }
            }
            if (fits) {
                blocks.push_back({ candidate, end, procName });
                // Re-sort after insert to keep order.
                std::sort(blocks.begin(), blocks.end(),
                    [](const MemBlock& a, const MemBlock& b) { return a.start < b.start; });
                startAddr = candidate;
                return true;
            }
        }
        return false; // memory full
    }

    // Releases the block held by procName.
    void free(const std::string& procName) override{
        std::lock_guard<std::mutex> lock(mtx);
        blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
            [&](const MemBlock& b) { return b.procName == procName; }),
            blocks.end());
    }

    // Returns true if procName currently has memory allocated.
    bool isAllocated(const std::string& procName) override{
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& b : blocks)
            if (b.procName == procName) return true;
        return false;
    }

    int processesInMemory() override {
        std::lock_guard<std::mutex> lock(mtx);
        return static_cast<int>(blocks.size());
    }

    // Total external fragmentation: free memory that exists but is too small
    // (< memPerProc) to satisfy a new allocation request.
    uint32_t externalFragmentation() override {
        std::lock_guard<std::mutex> lock(mtx);

        // Collect all free gaps.
        std::vector<uint32_t> gaps;
        uint32_t prev = 0;
        for (auto& b : blocks) {
            if (b.start > prev)
                gaps.push_back(b.start - prev);
            prev = b.end + 1;
        }
        if (prev < totalMem)
            gaps.push_back(totalMem - prev);

        // External fragmentation = free gaps that are smaller than memPerProc.
        uint32_t frag = 0;
        for (uint32_t g : gaps)
            if (g < memPerProc) frag += g;
        return frag;
    }

    static std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_info;
#ifdef _WIN32
        localtime_s(&tm_info, &t);
#else
        std::tm* tmp = std::localtime(&t);
        if (tmp) tm_info = *tmp;
#endif
        std::ostringstream oss;
        oss << "(" << std::setfill('0')
            << std::setw(2) << (tm_info.tm_mon + 1) << "/"
            << std::setw(2) << tm_info.tm_mday << "/"
            << (tm_info.tm_year + 1900) << " "
            << std::setw(2) << tm_info.tm_hour << ":"
            << std::setw(2) << tm_info.tm_min << ":"
            << std::setw(2) << tm_info.tm_sec;
        oss << (tm_info.tm_hour < 12 ? "AM)" : "PM)");
        return oss.str();
    }

    // Writes memory_stamp_<qq>.txt per the spec's ASCII layout.
    // Format (top = high address, bottom = low address):
    //   Timestamp: ...
    //   Number of processes in memory: N
    //   Total external fragmentation in KB: F
    //
    //   ----end---- = <totalMem>
    //   <upper addr of topmost block>
    //   <procName>
    //   <lower addr of topmost block>
    //   ...
    //   ----start----- = 0
    void writeSnapshot(uint64_t quantumCycle) override{
        std::lock_guard<std::mutex> lock(mtx);

        std::string filename = "memory_stamp_" +
            std::to_string(quantumCycle) + ".txt";
        std::ofstream out(filename);
        if (!out.is_open()) return;

        // Header
        out << "Timestamp: " << getCurrentTimestamp() << "\n";
        out << "Number of processes in memory: " << blocks.size() << "\n";

        // Fragmentation (compute without locking since we hold the lock already)
        uint32_t prev = 0;
        uint32_t frag = 0;
        for (auto& b : blocks) {
            if (b.start > prev) {
                uint32_t gap = b.start - prev;
                if (gap < memPerProc) frag += gap;
            }
            prev = b.end + 1;
        }
        if (prev < totalMem) {
            uint32_t gap = totalMem - prev;
            if (gap < memPerProc) frag += gap;
        }
        out << "Total external fragmentation in KB: " << (frag / 1024) << "\n\n";

        // ASCII memory map, top (high address) to bottom (low address)
        out << "----end---- = " << totalMem << "\n";

        // Print blocks from highest to lowest
        for (int i = static_cast<int>(blocks.size()) - 1; i >= 0; i--) {
            auto& b = blocks[i];
            out << b.end + 1 << "\n";
            out << b.procName << "\n";
            out << b.start << "\n";
        }

        out << "----start----- = 0\n";
        out.close();
    }
};
#pragma once
