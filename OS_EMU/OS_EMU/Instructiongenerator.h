// InstructionGenerator.h
#pragma once
#include "Instruction.h"
#include <random>
#include <vector>
#include <string>

// Builds a randomized instruction list for a scheduler-generated process:
// a mix of PRINT/DECLARE/ADD/SUBTRACT/SLEEP, with occasional (nested, up to
// maxDepth deep) FOR loops.
class InstructionGenerator {
public:
    InstructionGenerator() : rng(std::random_device{}()) {}

    // Build the instruction list for a process with `count` instructions.
    // `depth` is used internally for recursive FOR generation.
    std::vector<Instruction> generate(const std::string& processName,
        int count,
        int depth = 0,
        int maxDepth = 3)
    {
        std::vector<Instruction> result;
        for (int i = 0; i < count; ++i) {
            int choice = rng() % 6;
            if (choice == 5 && depth < maxDepth) {
                Instruction forIns;
                forIns.type = InstructionType::FOR;
                forIns.forRepeats = 2 + (rng() % 3);
                int bodySize = 1 + (rng() % 3);
                forIns.forBody = generate(processName, bodySize, depth + 1, maxDepth);
                result.push_back(forIns);
            }
            else {
                switch (choice % 5) {
                case 0: result.push_back(makePrint(processName));               break;
                case 1: result.push_back(makeDeclare());                        break;
                case 2: result.push_back(makeArith(InstructionType::ADD));      break;
                case 3: result.push_back(makeArith(InstructionType::SUBTRACT)); break;
                case 4: result.push_back(makeSleep());                         break;
                }
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

    std::string randomVarName() {
        static const char* names[] = { "x", "y", "z", "a", "b" };
        return names[rng() % 5];
    }
};