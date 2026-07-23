// IMemoryAllocator.h
#pragma once
#include <string>
#include <cstddef>

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
