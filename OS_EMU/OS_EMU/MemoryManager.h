//MemoryManager.h
#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdint>
#include "Process.h"

// A single allocated region of main memory belonging to one process.
// Addresses are byte offsets in [0, maxOverallMem).
struct MemBlock {
    size_t start;      // inclusive
    size_t end;        // exclusive
    std::string procName;
    int pid;
};

// Flat, contiguous, first-fit memory allocator.
// Every process requires exactly `memPerProc` bytes. A process keeps its
// block for its entire lifetime (no paging / no backing store); the block is
// released only when the process finishes.
class MemoryManager {
public:
    MemoryManager(size_t maxOverallMem, size_t memPerFrame, size_t memPerProc)
        : totalMemory(maxOverallMem), frameSize(memPerFrame), procSize(memPerProc) {
    }

    // Attempts to place a block of size `procSize` using first-fit search
    // over the free gaps between currently allocated blocks (and the tail
    // gap up to totalMemory). Returns true on success.
    bool allocate(const std::string& name, int pid) {
        std::lock_guard<std::mutex> lock(mtx);

        std::sort(blocks.begin(), blocks.end(),
            [](const MemBlock& a, const MemBlock& b) { return a.start < b.start; });

        size_t prevEnd = 0;
        for (auto& b : blocks) {
            if (b.start - prevEnd >= procSize) {
                blocks.push_back({ prevEnd, prevEnd + procSize, name, pid });
                return true;
            }
            prevEnd = b.end;
        }
        if (totalMemory - prevEnd >= procSize) {
            blocks.push_back({ prevEnd, prevEnd + procSize, name, pid });
            return true;
        }
        return false; // no fit: caller should requeue the process
    }

    // Frees the block belonging to `pid`. No-op if not present.
    void deallocate(int pid) {
        std::lock_guard<std::mutex> lock(mtx);
        blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
            [pid](const MemBlock& b) { return b.pid == pid; }), blocks.end());
    }

    int getProcessCount() {
        std::lock_guard<std::mutex> lock(mtx);
        return static_cast<int>(blocks.size());
    }

    // Sum of every free gap (including the tail gap). This is what the
    // assignment mockup labels "Total external fragmentation in KB"
    // (the raw byte count, per the given example).
    size_t getExternalFragmentation() {
        std::lock_guard<std::mutex> lock(mtx);
        return computeFragmentationLocked();
    }

    // Writes "memory_stamp_<qq>.txt" in the mockup layout:
    //   Timestamp: (...)
    //   Number of processes in memory: N
    //   Total external fragmentation in KB: F
    //   ----end---- = total
    //   <end addr>
    //   <procName>
    //   <start addr>
    //   ... (repeated top-down, highest address first)
    //   ----start---- = 0
    void writeSnapshot(int quantumCycle) {
        std::lock_guard<std::mutex> lock(mtx);

        std::vector<MemBlock> sorted = blocks;
        std::sort(sorted.begin(), sorted.end(),
            [](const MemBlock& a, const MemBlock& b) { return a.start < b.start; });

        size_t frag = computeFragmentationLocked();

        std::ostringstream oss;
        oss << "Timestamp: " << Process::getCurrentTimestamp() << "\n";
        oss << "Number of processes in memory: " << sorted.size() << "\n";
        oss << "Total external fragmentation in KB: " << frag << "\n";
        oss << "----end---- = " << totalMemory << "\n";

        // Print top-down: highest address first.
        for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
            oss << it->end << "\n";
            oss << it->procName << "\n";
            oss << it->start << "\n";
        }
        oss << "----start---- = 0\n";

        std::ostringstream fname;
        fname << "memory_stamp_" << std::setfill('0') << std::setw(2) << quantumCycle << ".txt";

        std::ofstream out(fname.str());
        if (out.is_open()) {
            out << oss.str();
            out.close();
        }
    }

private:
    size_t totalMemory;
    size_t frameSize;
    size_t procSize;
    std::vector<MemBlock> blocks;
    std::mutex mtx;

    // Caller must hold mtx.
    size_t computeFragmentationLocked() {
        std::vector<MemBlock> sorted = blocks;
        std::sort(sorted.begin(), sorted.end(),
            [](const MemBlock& a, const MemBlock& b) { return a.start < b.start; });

        size_t free = 0;
        size_t prevEnd = 0;
        for (auto& b : sorted) {
            free += (b.start - prevEnd);
            prevEnd = b.end;
        }
        free += (totalMemory - prevEnd);
        return free;
    }
};
