// PagingAllocator.h
#pragma once
#include "IMemoryAllocator.h"
#include <vector>
#include <queue>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <cstdint>
#include <atomic>
#include <algorithm>

// True demand-paging allocator.
//
// allocate(size) ONLY builds a page table (all entries invalid) — it does
// NOT touch physical frames. Frames are assigned lazily, one page at a
// time, the first time that page is actually referenced by a READ/WRITE
// via ensurePageResident() (called from readUint16/writeUint16). This is
// what makes it "demand" paging rather than paging done up front.
//
// Physical memory itself is simulated as frameData: numFrames buffers of
// frameSize bytes each. A page's bytes live in frameData[frame] while
// resident, and get copied out to backingStore (and to
// csopesy-backing-store.txt) when evicted; they get copied back in on the
// next reference (a page fault).
class PagingAllocator : public IMemoryAllocator
{
private:
    struct PageTableEntry
    {
        bool valid = false;          // resident in a physical frame right now?
        int  frameNumber = -1;       // which physical frame, if valid
        bool everSwappedOut = false; // has this page ever been written to the backing store?
    };

    struct Allocation
    {
        size_t requestedSize;
        std::vector<PageTableEntry> pageTable;
    };

    size_t frameSize;
    int    numFrames;

    std::vector<bool> frameOccupied;
    std::vector<int>  frameOwnerAllocId; // which allocation id owns frame i (-1 if free)
    std::vector<std::vector<uint8_t>> frameData; // the simulated physical bytes

    std::queue<int> fifoFrameOrder;

    std::unordered_map<intptr_t, Allocation> allocations;
    intptr_t nextAllocId = 1;

    // Backing store: key = (allocId << 32) | pageIndex -> that page's bytes.
    std::unordered_map<uint64_t, std::vector<uint8_t>> backingStore;
    static uint64_t makeKey(intptr_t id, size_t pageIndex)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(id)) << 32)
            | static_cast<uint32_t>(pageIndex);
    }

    std::atomic<int> pagesPagedIn{ 0 };
    std::atomic<int> pagesPagedOut{ 0 };

    std::mutex mtx;

    int findFreeFrame()
    {
        for (int i = 0; i < numFrames; i++)
            if (!frameOccupied[i]) return i;
        return -1;
    }

    // FIFO eviction: picks the oldest-assigned frame, writes its content to
    // the backing store, and frees it.
    bool evictVictim()
    {
        while (!fifoFrameOrder.empty())
        {
            int frame = fifoFrameOrder.front();
            fifoFrameOrder.pop();

            if (!frameOccupied[frame]) continue; // stale entry, already freed

            intptr_t ownerId = frameOwnerAllocId[frame];
            auto ait = allocations.find(ownerId);
            if (ait == allocations.end())
            {
                // Owner no longer exists (shouldn't normally happen, but don't
                // leak the frame if it does).
                frameOccupied[frame] = false;
                frameOwnerAllocId[frame] = -1;
                continue;
            }

            auto& table = ait->second.pageTable;
            for (size_t p = 0; p < table.size(); p++)
            {
                auto& pte = table[p];
                if (pte.valid && pte.frameNumber == frame)
                {
                    backingStore[makeKey(ownerId, p)] = frameData[frame];
                    pte.valid = false;
                    pte.frameNumber = -1;
                    pte.everSwappedOut = true;

                    frameOccupied[frame] = false;
                    frameOwnerAllocId[frame] = -1;
                    pagesPagedOut++;
                    writeBackingStoreFile();
                    return true;
                }
            }
        }
        return false;
    }

    // Loads a page's bytes into 'frame'. If it was never swapped out before,
    // it's brand new memory, so it's zero-filled (per spec: an uninitialized
    // memory block reads as 0).
    void loadIntoFrame(intptr_t id, size_t pageIndex, int frame, bool everSwappedOut)
    {
        if (everSwappedOut)
        {
            auto it = backingStore.find(makeKey(id, pageIndex));
            if (it != backingStore.end())
            {
                frameData[frame] = it->second;
                backingStore.erase(it);
                writeBackingStoreFile();
            }
            else
            {
                std::fill(frameData[frame].begin(), frameData[frame].end(), 0);
            }
            pagesPagedIn++;
        }
        else
        {
            std::fill(frameData[frame].begin(), frameData[frame].end(), 0);
        }
    }

    // The actual page-fault path: called whenever an address on this page is
    // referenced. Assigns a frame (evicting a victim if none are free) and
    // brings the page's content in. Returns false only if memory is
    // exhausted and no victim could be found (should not normally happen —
    // there's always at least one frame across all processes to evict from).
    bool ensurePageResident(intptr_t id, Allocation& alloc, size_t pageIndex)
    {
        PageTableEntry& pte = alloc.pageTable[pageIndex];
        if (pte.valid) return true;

        int frame = findFreeFrame();
        if (frame == -1)
        {
            if (!evictVictim()) return false;
            frame = findFreeFrame();
            if (frame == -1) return false;
        }

        loadIntoFrame(id, pageIndex, frame, pte.everSwappedOut);

        pte.valid = true;
        pte.frameNumber = frame;
        frameOccupied[frame] = true;
        frameOwnerAllocId[frame] = static_cast<int>(id);
        fifoFrameOrder.push(frame);
        return true;
    }

    // Reads/writes a single byte at a virtual address, faulting the owning
    // page in first if needed. Caller has already bounds-checked address
    // against alloc.requestedSize.
    bool accessByte(intptr_t id, Allocation& alloc, uint32_t address, uint8_t* readOut, const uint8_t* writeIn)
    {
        size_t pageIndex = address / frameSize;
        size_t offset = address % frameSize;
        if (pageIndex >= alloc.pageTable.size()) return false;
        if (!ensurePageResident(id, alloc, pageIndex)) return false;

        int frame = alloc.pageTable[pageIndex].frameNumber;
        if (readOut) *readOut = frameData[frame][offset];
        if (writeIn) frameData[frame][offset] = *writeIn;
        return true;
    }

    void writeBackingStoreFile()
    {
        std::ofstream out("csopesy-backing-store.txt");
        if (!out.is_open()) return;
        for (auto& kv : backingStore)
        {
            intptr_t id = static_cast<intptr_t>(kv.first >> 32);
            uint32_t page = static_cast<uint32_t>(kv.first & 0xFFFFFFFFu);
            out << "proc#" << id << " page#" << page << " :";
            for (uint8_t b : kv.second)
                out << " " << std::hex << std::setw(2) << std::setfill('0') << (int)b;
            out << std::dec << "\n";
        }
        out.close();
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
        frameData.assign(numFrames, std::vector<uint8_t>(frameSize, 0));
    }

    // Only builds the page table (all pages invalid) — no frames are
    // touched here. This is the "on-demand" part: real allocation happens
    // lazily per-page on first reference (see ensurePageResident).
    void* allocate(size_t size) override
    {
        std::lock_guard<std::mutex> lock(mtx);

        int pagesNeeded = static_cast<int>((size + frameSize - 1) / frameSize); // ceil
        if (pagesNeeded > numFrames)
            return nullptr; // can never fit, even with the whole machine free

        Allocation alloc;
        alloc.requestedSize = size;
        alloc.pageTable.resize(pagesNeeded);

        intptr_t id = nextAllocId++;
        allocations[id] = std::move(alloc);

        return reinterpret_cast<void*>(id);
    }

    void deallocate(void* ptr) override
    {
        std::lock_guard<std::mutex> lock(mtx);

        intptr_t id = reinterpret_cast<intptr_t>(ptr);
        auto it = allocations.find(id);
        if (it == allocations.end()) return;

        for (auto& pte : it->second.pageTable)
        {
            if (pte.valid)
            {
                frameOccupied[pte.frameNumber] = false;
                frameOwnerAllocId[pte.frameNumber] = -1;
            }
        }

        // Purge any pages of this process still sitting in the backing store.
        for (auto bit = backingStore.begin(); bit != backingStore.end(); )
        {
            if (static_cast<intptr_t>(static_cast<uint32_t>(bit->first >> 32)) == id)
                bit = backingStore.erase(bit);
            else
                ++bit;
        }

        allocations.erase(it);
        writeBackingStoreFile();
    }

    // --- Uniform memory-access interface (IMemoryAllocator) ---
    bool readUint16(void* h, uint32_t address, uint16_t& outValue) override
    {
        std::lock_guard<std::mutex> lock(mtx);
        intptr_t id = reinterpret_cast<intptr_t>(h);
        auto it = allocations.find(id);
        if (it == allocations.end()) return false;
        Allocation& alloc = it->second;

        if (static_cast<size_t>(address) + 2 > alloc.requestedSize) return false; // access violation

        uint8_t b0 = 0, b1 = 0;
        if (!accessByte(id, alloc, address, &b0, nullptr)) return false;
        if (!accessByte(id, alloc, address + 1, &b1, nullptr)) return false;
        outValue = static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
        return true;
    }

    bool writeUint16(void* h, uint32_t address, uint16_t value) override
    {
        std::lock_guard<std::mutex> lock(mtx);
        intptr_t id = reinterpret_cast<intptr_t>(h);
        auto it = allocations.find(id);
        if (it == allocations.end()) return false;
        Allocation& alloc = it->second;

        if (static_cast<size_t>(address) + 2 > alloc.requestedSize) return false; // access violation

        uint8_t lo = static_cast<uint8_t>(value & 0xFF);
        uint8_t hi = static_cast<uint8_t>((value >> 8) & 0xFF);
        if (!accessByte(id, alloc, address, nullptr, &lo)) return false;
        if (!accessByte(id, alloc, address + 1, nullptr, &hi)) return false;
        return true;
    }

    int processesInMemory()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return static_cast<int>(allocations.size());
    }

    size_t externalFragmentation() { return 0; } // paging has no external fragmentation by design

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

    // Used memory = bytes actually resident in physical frames right now
    // (as opposed to requestedSize, which counts pages never yet touched).
    size_t getUsedMemory() override
    {
        std::lock_guard<std::mutex> lock(mtx);
        int occ = 0;
        for (bool b : frameOccupied) if (b) occ++;
        return static_cast<size_t>(occ) * frameSize;
    }

    String visualizeMemory() override
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::ostringstream out;
        out << "Frames: " << numFrames << " (frame size " << frameSize << " bytes)\n";
        for (int i = 0; i < numFrames; i++)
        {
            out << "  [" << i << "] "
                << (frameOccupied[i] ? ("proc#" + std::to_string(frameOwnerAllocId[i])) : "free")
                << "\n";
        }
        return out.str();
    }
};