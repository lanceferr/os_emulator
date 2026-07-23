// MemoryAllocator.h
#pragma once
#include "IMemoryAllocator.h"
#include <vector>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <unordered_map>

class MemoryAllocator : public IMemoryAllocator
{
private:
    std::vector<MemoryBlock> blocks; // kept sorted by start; MemoryBlock IS used here
    std::unordered_map<intptr_t, size_t> handleToStart; // void* handle -> block start
    intptr_t nextHandle = 1;
    std::mutex mtx;
    std::unordered_map<std::string, intptr_t> nameToHandle; // process name -> handle
    size_t memPerProc = 0;

public:
    MemoryAllocator(size_t totalMem, size_t memPerProc, size_t /*memPerFrame*/)
    {
        memoryAllocatorType = FLAT_MEMORY_ALLOCATOR;
        maximumSize = totalMem;
        currentAllocatedSize = 0;
        this->memPerProc = memPerProc;
    }

    // First-fit contiguous allocation.
    void* allocate(size_t size) override
    {
        std::lock_guard<std::mutex> lock(mtx);

        std::sort(blocks.begin(), blocks.end()); // uses MemoryBlock::operator<

        std::vector<size_t> candidates{ 0 };
        for (auto& b : blocks)
            candidates.push_back(b.start + b.size);

        for (size_t candidateStart : candidates)
        {
            size_t candidateEnd = candidateStart + size;
            if (candidateEnd > maximumSize) break;

            bool fits = true;
            for (auto& b : blocks)
            {
                size_t bEnd = b.start + b.size;
                if (!(candidateEnd <= b.start || candidateStart >= bEnd))
                {
                    fits = false;
                    break;
                }
            }
            if (fits)
            {
                blocks.push_back({ candidateStart, size });
                std::sort(blocks.begin(), blocks.end());

                intptr_t handle = nextHandle++;
                handleToStart[handle] = candidateStart;
                currentAllocatedSize += size;
                return reinterpret_cast<void*>(handle);
            }
        }
        return nullptr; // no fit found
    }

    void deallocate(void* ptr) override
    {
        std::lock_guard<std::mutex> lock(mtx);

        intptr_t handle = reinterpret_cast<intptr_t>(ptr);
        auto it = handleToStart.find(handle);
        if (it == handleToStart.end()) return;

        size_t start = it->second;
        auto blockIt = std::find_if(blocks.begin(), blocks.end(),
            [&](const MemoryBlock& b) { return b.start == start; });
        if (blockIt != blocks.end())
        {
            currentAllocatedSize -= blockIt->size;
            blocks.erase(blockIt);
        }
        handleToStart.erase(it);
    }

    // Snapshot: write a simple textual dump to mem-snap-<quantum>.txt
    void writeSnapshot(uint64_t quantumCounter)
    {
        std::ostringstream fname;
        fname << "mem-snap-" << quantumCounter << ".txt";
        std::ofstream out(fname.str());
        if (out.is_open()) {
            out << visualizeMemory();
            out.close();
        }
    }

    int processesInMemory()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return static_cast<int>(nameToHandle.size());
    }

    size_t externalFragmentation()
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (blocks.empty()) return maximumSize;
        std::sort(blocks.begin(), blocks.end());
        size_t largestFree = 0;
        size_t prevEnd = 0;
        for (auto& b : blocks) {
            if (b.start > prevEnd) {
                size_t gap = b.start - prevEnd;
                if (gap > largestFree) largestFree = gap;
            }
            prevEnd = b.start + b.size;
        }
        if (prevEnd < maximumSize) {
            size_t gap = maximumSize - prevEnd;
            if (gap > largestFree) largestFree = gap;
        }
        size_t totalFree = maximumSize - currentAllocatedSize;
        if (totalFree <= largestFree) return 0;
        return totalFree - largestFree;
    }

    // Optional: track allocation by process name (not used by Scheduler now)
    bool allocateForProcess(const std::string& procName)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (nameToHandle.find(procName) != nameToHandle.end()) return false;
        void* h = allocate(memPerProc);
        if (!h) return false;
        intptr_t handle = reinterpret_cast<intptr_t>(h);
        nameToHandle[procName] = handle;
        return true;
    }

    void freeProcess(const std::string& procName)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = nameToHandle.find(procName);
        if (it == nameToHandle.end()) return;
        intptr_t handle = it->second;
        nameToHandle.erase(it);
        deallocate(reinterpret_cast<void*>(handle));
    }

    String visualizeMemory() override
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::sort(blocks.begin(), blocks.end());
        std::ostringstream out;
        out << "----end---- = " << maximumSize << "\n";
        for (auto it = blocks.rbegin(); it != blocks.rend(); ++it)
            out << (it->start + it->size) << "\n" << it->start << "\n";
        out << "----start----- = 0\n";
        return out.str();
    }
};
