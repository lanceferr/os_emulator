// IMemoryAllocator.h
#pragma once
#include <string>
#include <cstdint>

class IMemoryAllocator {
public:
    virtual ~IMemoryAllocator() = default;

    virtual bool allocate(const std::string& procName, uint32_t& startAddr) = 0;
    virtual void free(const std::string& procName) = 0;
    virtual bool isAllocated(const std::string& procName) = 0;
    virtual int  processesInMemory() = 0;
    virtual uint32_t externalFragmentation() = 0;
    virtual void writeSnapshot(uint64_t quantumCycle) = 0;
};