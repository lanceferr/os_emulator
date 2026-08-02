// InstructionGenerator.h
#pragma once
#include "Instruction.h"
#include <random>
#include <vector>
#include <string>

// Builds a randomized instruction list for a scheduler-generated process:
// a mix of PRINT/DECLARE/ADD/SUBTRACT/SLEEP/READ/WRITE, with occasional
// (nested, up to maxDepth deep) FOR loops.
//
// memSize is the process's allocated memory size (bytes). When >= 2, READ
// and WRITE instructions are included in the mix, targeting addresses in
// [0, memSize - 2] so they stay within the process's own allocation and
// don't trip an access violation. This is what drives demand paging for
// scheduler-generated processes: without READ/WRITE, no page is ever
// referenced, so PagingAllocator never faults a page in (see
// PagingAllocator::ensurePageResident, only called from accessByte, only
// called from readUint16/writeUint16).
class InstructionGenerator {
public:
    InstructionGenerator() : rng(std::random_device{}()) {}

    // Build the instruction list for a process with `count` instructions.
    // `depth` is used internally for recursive FOR generation.
    std::vector<Instruction> generate(const std::string& processName,
        int count,
        size_t memSize = 0,
        int depth = 0,
        int maxDepth = 3)
    {
        std::vector<Instruction> result;
        bool allowMemOps = memSize >= 2;

        for (int i = 0; i < count; ++i) {
            // Build the set of choices available this iteration.
            // 0=PRINT 1=DECLARE 2=ADD 3=SUBTRACT 4=SLEEP 5=FOR 6=READ 7=WRITE
            std::vector<int> choices = { 0, 1, 2, 3, 4 };
            if (depth < maxDepth) choices.push_back(5);
            if (allowMemOps) { choices.push_back(6); choices.push_back(7); }

            int choice = choices[rng() % choices.size()];

            switch (choice) {
            case 0: result.push_back(makePrint(processName));               break;
            case 1: result.push_back(makeDeclare());                        break;
            case 2: result.push_back(makeArith(InstructionType::ADD));      break;
            case 3: result.push_back(makeArith(InstructionType::SUBTRACT)); break;
            case 4: result.push_back(makeSleep());                         break;
            case 5: {
                Instruction forIns;
                forIns.type = InstructionType::FOR;
                forIns.forRepeats = 2 + (rng() % 3);
                int bodySize = 1 + (rng() % 3);
                forIns.forBody = generate(processName, bodySize, memSize, depth + 1, maxDepth);
                result.push_back(forIns);
                break;
            }
            case 6: result.push_back(makeRead(memSize));                    break;
            case 7: result.push_back(makeWrite(memSize));                   break;
            }
        }
        return result;
    }

private:
    std::mt19937 rng;

    Instruction makePrint(const std::string& processName) {
        Instruction ins;
        ins.type = InstructionType::PRINT;
        ins.printLiteralMsg = "Hello world from " + processName + "!";
        ins.printVarName = "";
        return ins;
    }

    Instruction makeDeclare() {
        Instruction ins;
        ins.type = InstructionType::DECLARE;
        ins.declareVar = randomVarName();
        ins.declareValue = static_cast<uint16_t>(rng() % 100);
        return ins;
    }

    Instruction makeArith(InstructionType opType) {
        Instruction ins;
        ins.type = opType;
        ins.arithDest = randomVarName();
        ins.arithSrc1 = Operand::fromLiteral(static_cast<uint16_t>(rng() % 50));
        ins.arithSrc2 = Operand::fromLiteral(static_cast<uint16_t>(rng() % 50));
        return ins;
    }

    Instruction makeSleep() {
        Instruction ins;
        ins.type = InstructionType::SLEEP;
        ins.sleepTicks = static_cast<uint8_t>(1 + (rng() % 5));
        return ins;
    }

    // Address must satisfy address + 2 <= memSize (readUint16/writeUint16's
    // bounds check), so valid addresses are [0, memSize - 2]. Caller only
    // invokes this when memSize >= 2, so memSize - 1 is a safe modulus.
    Instruction makeRead(size_t memSize) {
        Instruction ins;
        ins.type = InstructionType::READ;
        ins.readVar = randomVarName();
        ins.readAddress = static_cast<uint32_t>(rng() % (memSize - 1));
        return ins;
    }

    Instruction makeWrite(size_t memSize) {
        Instruction ins;
        ins.type = InstructionType::WRITE;
        ins.writeAddress = static_cast<uint32_t>(rng() % (memSize - 1));
        ins.writeValue = Operand::fromLiteral(static_cast<uint16_t>(rng() % 100));
        return ins;
    }

    std::string randomVarName() {
        static const char* names[] = { "x", "y", "z", "a", "b" };
        return names[rng() % 5];
    }
};