// PagingAllocator.h
#pragma once
#include "IMemoryAllocator.h"
#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>

// One entry per virtual page of a process.
struct PageTableEntry {
    bool valid = false;          // is this page currently resident in a physical frame?
    int  frameNumber = -1;       // which physical frame, if valid
    bool everSwappedOut = false; // has this page ever been written to the backing store?
                                  // (used to decide whether a reload counts as a "page-in")
};

class PagingAllocator : public IMemoryAllocator {
private:
    uint32_t totalMem;
    uint32_t memPerProc;
    uint32_t frameSize;
    int numFrames;
    int pagesPerProc; // ceil(memPerProc / frameSize)

    std::vector<bool>        frameOccupied; // frame table: is frame i in use?
    std::vector<std::string> frameOwner;    // frame table: which process owns frame i

    // FIFO order of frame assignment, used to pick an eviction victim.
    // Lazily cleaned: an index here may already have been freed elsewhere,
    // in which case it's just skipped when popped.
    std::queue<int> fifoFrameOrder;

    // Per-process page table.
    std::unordered_map<std::string, std::vector<PageTableEntry>> pageTables;

    // Simulated backing store: procName+pageIndex -> "on disk" marker.
    // We don't store real page bytes (this is a simulator), just the fact
    // that this page currently lives on the backing store rather than in a frame.
    std::unordered_set<std::string> backingStore;
    std::ofstream backingStoreLog;

    uint64_t pagesPagedIn = 0;
    uint64_t pagesPagedOut = 0;

    std::mutex mtx;

    static std::string key(const std::string& procName, int pageIndex) {
        return procName + "#" + std::to_string(pageIndex);
    }

    // Finds a free frame, or -1 if none exist right now.
    int findFreeFrame() {
        for (int i = 0; i < numFrames; i++)
            if (!frameOccupied[i]) return i;
        return -1;
    }

    // Evicts exactly one frame (FIFO) to the backing store, freeing it up.
    // Returns true if a frame was evicted, false if there was nothing to evict
    // (e.g. every frame is already free, which shouldn't happen if this is
    // only called when findFreeFrame() fails, but we guard anyway).
    bool evictVictim() {
        while (!fifoFrameOrder.empty()) {
            int frame = fifoFrameOrder.front();
            fifoFrameOrder.pop();

            if (!frameOccupied[frame]) continue; // stale entry, already freed

            std::string owner = frameOwner[frame];
            auto& table = pageTables[owner];

            // Find which page of `owner` maps to this frame.
            for (int p = 0; p < (int)table.size(); p++) {
                if (table[p].valid && table[p].frameNumber == frame) {
                    // "Write" the page out to the backing store.
                    backingStore.insert(key(owner, p));
                    if (backingStoreLog.is_open()) {
                        backingStoreLog << getCurrentTimestamp() << " OUT "
                                         << owner << " page " << p
                                         << " (was frame " << frame << ")\n";
                        backingStoreLog.flush();
                    }
                    table[p].valid = false;
                    table[p].frameNumber = -1;
                    table[p].everSwappedOut = true;
                    pagesPagedOut++;

                    frameOccupied[frame] = false;
                    frameOwner[frame] = "";
                    return true;
                }
            }
        }
        return false;
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

public:
    PagingAllocator(uint32_t totalMem, uint32_t memPerProc, uint32_t frameSize)
        : totalMem(totalMem), memPerProc(memPerProc), frameSize(frameSize) {
        numFrames = static_cast<int>(totalMem / frameSize);
        pagesPerProc = static_cast<int>((memPerProc + frameSize - 1) / frameSize); // ceil
        frameOccupied.assign(numFrames, false);
        frameOwner.assign(numFrames, "");

        backingStoreLog.open("csopesy-backing-store.txt", std::ios::app);
    }

    ~PagingAllocator() {
        if (backingStoreLog.is_open()) backingStoreLog.close();
    }

    // Allocates pagesPerProc frames for procName, evicting victims via FIFO
    // if memory is full. Returns false only if the process could never fit
    // even with the entire machine to itself (pagesPerProc > numFrames).
    bool allocate(const std::string& procName, uint32_t& startAddr) override {
        std::lock_guard<std::mutex> lock(mtx);

        if (pageTables.count(procName)) {
            // Already allocated; treat as success (idempotent).
            startAddr = 0;
            return true;
        }
        if (pagesPerProc > numFrames) {
            return false; // can never fit, regardless of eviction
        }

        std::vector<PageTableEntry> table(pagesPerProc);

        for (int p = 0; p < pagesPerProc; p++) {
            int frame = findFreeFrame();
            if (frame == -1) {
                if (!evictVictim()) {
                    // Nothing left to evict but still no free frame: bail out
                    // and undo the partial allocation so we don't leak frames.
                    for (int q = 0; q < p; q++) {
                        frameOccupied[table[q].frameNumber] = false;
                        frameOwner[table[q].frameNumber] = "";
                    }
                    return false;
                }
                frame = findFreeFrame();
            }

            frameOccupied[frame] = true;
            frameOwner[frame] = procName;
            fifoFrameOrder.push(frame);

            table[p].valid = true;
            table[p].frameNumber = frame;

            // Only counts as a "page-in" if this page previously existed on
            // the backing store (i.e. it's a reload, not a first-time load).
            std::string k = key(procName, p);
            if (backingStore.count(k)) {
                backingStore.erase(k);
                pagesPagedIn++;
                if (backingStoreLog.is_open()) {
                    backingStoreLog << getCurrentTimestamp() << " IN  "
                                     << procName << " page " << p
                                     << " (into frame " << frame << ")\n";
                    backingStoreLog.flush();
                }
            }
        }

        pageTables[procName] = std::move(table);
        startAddr = 0; // no real virtual addressing modeled yet; presence is what matters
        return true;
    }

    void free(const std::string& procName) override {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = pageTables.find(procName);
        if (it == pageTables.end()) return;

        for (auto& pte : it->second) {
            if (pte.valid) {
                frameOccupied[pte.frameNumber] = false;
                frameOwner[pte.frameNumber] = "";
            }
        }
        // Also drop any pages of this process still sitting in the backing store.
        for (int p = 0; p < (int)it->second.size(); p++)
            backingStore.erase(key(procName, p));

        pageTables.erase(it);
    }

    bool isAllocated(const std::string& procName) override {
        std::lock_guard<std::mutex> lock(mtx);
        return pageTables.count(procName) > 0;
    }

    int processesInMemory() override {
        std::lock_guard<std::mutex> lock(mtx);
        return static_cast<int>(pageTables.size());
    }

    // Paging has no external fragmentation by design: any free frame can
    // satisfy any request, so there's never a "gap too small to use."
    uint32_t externalFragmentation() override {
        return 0;
    }

    // Internal fragmentation: the wasted tail of each process's last page.
    // Not part of IMemoryAllocator, but handy for report-util.
    uint32_t internalFragmentation() {
        std::lock_guard<std::mutex> lock(mtx);
        uint32_t waste = 0;
        uint32_t remainder = memPerProc % frameSize;
        if (remainder != 0) {
            uint32_t perProcWaste = frameSize - remainder;
            waste = static_cast<uint32_t>(pageTables.size()) * perProcWaste;
        }
        return waste;
    }

    uint64_t getPagesPagedIn() {
        std::lock_guard<std::mutex> lock(mtx);
        return pagesPagedIn;
    }

    uint64_t getPagesPagedOut() {
        std::lock_guard<std::mutex> lock(mtx);
        return pagesPagedOut;
    }

    // Writes memory_stamp_<qq>.txt as a per-frame map instead of the
    // per-block map the flat allocator uses, since frames (not contiguous
    // blocks) are the unit of allocation under paging.
    void writeSnapshot(uint64_t quantumCycle) override {
        std::lock_guard<std::mutex> lock(mtx);

        std::string filename = "memory_stamp_" + std::to_string(quantumCycle) + ".txt";
        std::ofstream out(filename);
        if (!out.is_open()) return;

        out << "Timestamp: " << getCurrentTimestamp() << "\n";
        out << "Number of processes in memory: " << pageTables.size() << "\n";
        out << "Frame size: " << frameSize << "  Total frames: " << numFrames << "\n";
        out << "Pages paged in: " << pagesPagedIn
            << "  Pages paged out: " << pagesPagedOut << "\n\n";

        out << "Frame table (frame -> owner):\n";
        for (int i = 0; i < numFrames; i++) {
            out << "  [" << i << "] "
                << (frameOccupied[i] ? frameOwner[i] : std::string("free"))
                << "\n";
        }
        out.close();
    }
};