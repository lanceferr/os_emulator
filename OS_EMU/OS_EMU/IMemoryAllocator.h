// IMemoryAllocator.h
#pragma once
#include <string>
#include <cstddef>
#include <cstdint>

using String = std::string;

class IMemoryAllocator
{
public:
    enum MemoryAllocatorType
    {
        FLAT_MEMORY_ALLOCATOR,
        PAGING,
    };

    virtual void* allocate(size_t size) = 0;
    virtual void deallocate(void* ptr) = 0;
    virtual String visualizeMemory() = 0;

    // Uniform memory-access interface used by Process for READ/WRITE.
    // 'handle' is whatever allocate() returned for that process.
    // address is a virtual address relative to the process's own allocation,
    // i.e. valid range is [0, allocatedSize). Returns false on access
    // violation (out of the process's allocated range); true on success.
    // Concrete schemes handle demand paging / fault-in internally.
    virtual bool readUint16(void* handle, uint32_t address, uint16_t& outValue) = 0;
    virtual bool writeUint16(void* handle, uint32_t address, uint16_t value) = 0;

    // Reporting helpers for process-smi / vmstat. Default getUsedMemory()
    // just returns the tracked currentAllocatedSize; PagingAllocator
    // overrides it since resident-frame bytes differ from requested bytes.
    size_t getTotalMemory() const { return maximumSize; }
    virtual size_t getUsedMemory() { return currentAllocatedSize; }

    virtual ~IMemoryAllocator() = default;

protected:
    MemoryAllocatorType memoryAllocatorType;
    struct MemoryBlock
    {
        size_t start;
        size_t size;

        bool operator<(const MemoryBlock& other) const
        {
            return start < other.start;
        }
    };

    size_t maximumSize;
    size_t currentAllocatedSize;
};