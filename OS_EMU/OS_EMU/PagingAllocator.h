// PagingAllocator.h
#pragma once
#include "IMemoryAllocator.h"
#include <vector>
#include <queue>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <cstdint>

class PagingAllocator : public IMemoryAllocator
{
private:
    // One entry per virtual page of a single allocation.
    struct PageTableEntry
    {
        bool valid = false;          // resident in a physical frame right now?
        int  frameNumber = -1;       // which physical frame, if valid
        bool everSwappedOut = false; // has this page ever hit the backing store?
    };

    // One allocation = one contiguous run of virtual pages handed out by a
    // single allocate() call. This replaces the old "procName" identity.
    struct Allocation
    {
        size_t requestedSize;                  // what the caller asked for
        std::vector<PageTableEntry> pageTable;  // this allocation's own page table
    };

    size_t   frameSize;
    int      numFrames;

    std::vector<bool> frameOccupied;
    std::vector<int>  frameOwnerAllocId; // which allocation id owns frame i (-1 if free)

    std::queue<int> fifoFrameOrder;

    // The "handle" returned to the caller is just an allocation id, smuggled
    // out as a void*. This is the piece that answers Q5: given that void*,
    // we can recover the Allocation and walk its page table to find every
    // physical frame that needs to be freed.
    std::unordered_map<intptr_t, Allocation> allocations;
    intptr_t nextAllocId = 1;
    std::unordered_map<std::string, intptr_t> nameToAllocId;

    // Statistics
    std::atomic<int> pagesPagedIn{0};
    std::atomic<int> pagesPagedOut{0};

    std::mutex mtx;

    int findFreeFrame()
    {
        for (int i = 0; i < numFrames; i++)
            if (!frameOccupied[i]) return i;
        return -1;
    }

    // FIFO eviction: pick the oldest-assigned frame, write its page out to
    // the (simulated) backing store, and free the frame for reuse.
    bool evictVictim()
    {
        while (!fifoFrameOrder.empty())
        {
            int frame = fifoFrameOrder.front();
            fifoFrameOrder.pop();

            if (!frameOccupied[frame]) continue; // stale entry, already freed

            intptr_t ownerId = frameOwnerAllocId[frame];
            auto& table = allocations[ownerId].pageTable;

            for (auto& pte : table)
            {
                if (pte.valid && pte.frameNumber == frame)
                {
                    // Backing store write would happen here (omitted: this
                    // is a simulator, we don't persist real bytes).
                    pte.valid = false;
                    pte.frameNumber = -1;
                    pte.everSwappedOut = true;

                    frameOccupied[frame] = false;
                    frameOwnerAllocId[frame] = -1;
                    return true;
                }
            }
        }
        return false;
    }

public:
    PagingAllocator(size_t totalMem, size_t /*memPerProc*/, size_t frameSize)
        : frameSize(frameSize)
    {
        memoryAllocatorType = PAGING;
        maximumSize = totalMem;
        currentAllocatedSize = 0;

        numFrames = static_cast<int>(totalMem / frameSize);
        frameOccupied.assign(numFrames, false);
        frameOwnerAllocId.assign(numFrames, -1);
    }

    // --- Q2: allocate ---
    // size is in bytes; we round up to whole frames (this is exactly where
    // internal fragmentation is introduced — see Q6).
    void* allocate(size_t size) override
    {
        std::lock_guard<std::mutex> lock(mtx);

        int pagesNeeded = static_cast<int>((size + frameSize - 1) / frameSize); // ceil
        if (pagesNeeded > numFrames)
            return nullptr; // can never fit, even with the whole machine free

        Allocation alloc;
        alloc.requestedSize = size;
        alloc.pageTable.resize(pagesNeeded);

        for (int p = 0; p < pagesNeeded; p++)
        {
            int frame = findFreeFrame();
            if (frame == -1)
            {
                if (!evictVictim())
                {
                    // Roll back the frames we already claimed this call.
                    for (int q = 0; q < p; q++)
                    {
                        int f = alloc.pageTable[q].frameNumber;
                        frameOccupied[f] = false;
                        frameOwnerAllocId[f] = -1;
                    }
                    return nullptr;
                }
                frame = findFreeFrame();
            }

            frameOccupied[frame] = true;
            fifoFrameOrder.push(frame);
            alloc.pageTable[p].valid = true;
            alloc.pageTable[p].frameNumber = frame;
        }

        intptr_t id = nextAllocId++;
        for (auto& pte : alloc.pageTable)
            frameOwnerAllocId[pte.frameNumber] = static_cast<int>(id);

        allocations[id] = std::move(alloc);
        currentAllocatedSize += static_cast<size_t>(pagesNeeded) * frameSize;

        // The handle we hand back to the caller IS the allocation id,
        // reinterpreted as a pointer. It carries no real address meaning —
        // it's just a lookup key we can reverse in deallocate().
        return reinterpret_cast<void*>(id);
    }

    // --- Q5: deallocate, i.e. void* -> PTE/frame mapping ---
    void deallocate(void* ptr) override
    {
        std::lock_guard<std::mutex> lock(mtx);

        intptr_t id = reinterpret_cast<intptr_t>(ptr);
        auto it = allocations.find(id);
        if (it == allocations.end())
            return; // unknown/already-freed handle

        Allocation& alloc = it->second;

        // Walk every PTE this allocation owns and free the backing frame.
        for (auto& pte : alloc.pageTable)
        {
            if (pte.valid)
            {
                frameOccupied[pte.frameNumber] = false;
                frameOwnerAllocId[pte.frameNumber] = -1;
            }
            // (a page currently swapped out has no frame to free — nothing
            // to do for it beyond dropping the PTE itself)
        }

        currentAllocatedSize -= static_cast<size_t>(alloc.pageTable.size()) * frameSize;
        allocations.erase(it);
    }

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
        return static_cast<int>(nameToAllocId.size());
    }

    size_t externalFragmentation() { return 0; }

    int getPagesPagedIn() { return pagesPagedIn.load(); }
    int getPagesPagedOut() { return pagesPagedOut.load(); }

    size_t internalFragmentation()
    {
        std::lock_guard<std::mutex> lock(mtx);
        size_t total = 0;
        for (auto& kv : allocations) {
            const Allocation& a = kv.second;
            size_t allocated = a.pageTable.size() * frameSize;
            if (allocated > a.requestedSize) total += (allocated - a.requestedSize);
        }
        return total;
    }

    String visualizeMemory() override
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::ostringstream out;
        out << "Frames: " << numFrames << " (frame size " << frameSize << " bytes)\n";
        for (int i = 0; i < numFrames; i++)
        {
            out << "  [" << i << "] "
                << (frameOccupied[i] ? ("alloc#" + std::to_string(frameOwnerAllocId[i])) : "free")
                << "\n";
        }
        return out.str();
    }
};
