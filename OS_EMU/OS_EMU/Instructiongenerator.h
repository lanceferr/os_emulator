//// InstructionGenerator.h
//#pragma once
//#include "Instruction.h"
//#include <random>
//#include <vector>
//#include <string>
//
//// ─────────────────────────────────────────────────────────────────────────────
//// GenerationMode controls what instruction set a process receives.
////
////   RANDOM   – original behaviour: random mix of PRINT/DECLARE/ADD/SUBTRACT/
////              SLEEP/FOR instructions.
////
////   XYZ_COUNTER – deterministic set required by the quiz test case:
////
////       FOR([
////           ADD  (x, x, 1),
////           PRINT("Value from: " +x),
////           ADD  (y, y, 1),
////           PRINT("Value from: " +y),
////           ADD  (z, z, 1),
////           PRINT("Value from: " +z)
////       ], 100)
////
////       Repeated enough times so the total flat instruction count falls in
////       [min-ins, max-ins].  (Each FOR expands to 100 * 6 = 600 flat
////       instructions, so we emit ceil(count / 600) FOR blocks.)
////
//// NOTE: regardless of mode, the Process constructor must pre-populate x, y, z
////       in its symbol table with value 0.  See Process.h / Process.cpp.
//// ─────────────────────────────────────────────────────────────────────────────
//
//enum class GenerationMode {
//    RANDOM,
//    XYZ_COUNTER
//};
//
//class InstructionGenerator {
//public:
//    InstructionGenerator() : rng(std::random_device{}()) {}
//
//    // ── Public interface ─────────────────────────────────────────────────────
//
//    // Set the active mode before calling generate().
//    void setMode(GenerationMode m) { mode = m; }
//    GenerationMode getMode() const { return mode; }
//
//    // Build the instruction list for a process with `count` instructions.
//    // `depth` is used internally for recursive FOR generation (RANDOM mode).
//    std::vector<Instruction> generate(const std::string& processName,
//        int count,
//        int depth = 0,
//        int maxDepth = 3)
//    {
//        /*if (mode == GenerationMode::XYZ_COUNTER)
//            return generateXYZ(count);
//        else*/
//            return generateRandom(processName, count, depth, maxDepth);
//    }
//
//private:
//    std::mt19937   rng;
//    GenerationMode mode = GenerationMode::RANDOM;
//
//    // ── XYZ_COUNTER mode ────────────────────────────────────────────────────
//
//    // Build the canonical FOR body:
//    //   ADD(x, x, 1), PRINT("Value from: " +x),
//    //   ADD(y, y, 1), PRINT("Value from: " +y),
//    //   ADD(z, z, 1), PRINT("Value from: " +z)
//    //static std::vector<Instruction> makeXYZBody() {
//    //    std::vector<Instruction> body;
//
//    //    for (const char* var : { "x", "y", "z" }) {
//    //        // ADD(var, var, 1)
//    //        Instruction add;
//    //        add.type = InstructionType::ADD;
//    //        add.arithDest = var;
//    //        add.arithSrc1 = Operand::fromVar(var);
//    //        add.arithSrc2 = Operand::fromLiteral(1);
//    //        body.push_back(add);
//
//    //        // PRINT("Value from: " +var)
//    //        Instruction pr;
//    //        pr.type = InstructionType::PRINT;
//    //        pr.printLiteralMsg = "Value from: ";
//    //        pr.printVarName = var;   // executor appends the variable's value
//    //        body.push_back(pr);
//    //    }
//    //    return body;
//    //}
//
//    //// One FOR block = 100 iterations × 6 body instructions = 600 flat instructions.
//    //static Instruction makeXYZFor() {
//    //    Instruction f;
//    //    f.type = InstructionType::FOR;
//    //    f.forRepeats = 100;
//    //    f.forBody = makeXYZBody();
//    //    return f;
//    //}
//
//    // Emit enough FOR blocks so flat count >= requested count.
//    // Each FOR block contributes (forRepeats * bodySize) = 600 flat instructions.
//    //std::vector<Instruction> generateXYZ(int count) {
//    //    const int flatPerBlock = 100 * 6; // 600
//    //    int blocks = (count + flatPerBlock - 1) / flatPerBlock; // ceil division
//    //    if (blocks < 1) blocks = 1;
//
//    //    std::vector<Instruction> result;
//    //    result.reserve(blocks);
//    //    for (int i = 0; i < blocks; ++i)
//    //        result.push_back(makeXYZFor());
//    //    return result;
//    //}
//
//    // ── RANDOM mode (original logic) ─────────────────────────────────────────
//
//    /*std::vector<Instruction> generateRandom(const std::string& processName,
//        int count,
//        int depth,
//        int maxDepth)
//    {
//        std::vector<Instruction> result;
//        for (int i = 0; i < count; ++i) {
//            int choice = rng() % 6;
//            if (choice == 5 && depth < maxDepth) {
//                Instruction forIns;
//                forIns.type = InstructionType::FOR;
//                forIns.forRepeats = 2 + (rng() % 3);
//                int bodySize = 1 + (rng() % 3);
//                forIns.forBody = generateRandom(processName, bodySize,
//                    depth + 1, maxDepth);
//                result.push_back(forIns);
//            }
//            else {
//                switch (choice % 5) {
//                case 0: result.push_back(makePrint(processName));              break;
//                case 1: result.push_back(makeDeclare());                       break;
//                case 2: result.push_back(makeArith(InstructionType::ADD));     break;
//                case 3: result.push_back(makeArith(InstructionType::SUBTRACT)); break;
//                case 4: result.push_back(makeSleep());                         break;
//                }
//            }
//        }
//        return result;
//    }
//
//    Instruction makePrint(const std::string& processName) {
//        Instruction ins;
//        ins.type = InstructionType::PRINT;
//        ins.printLiteralMsg = "Hello world from " + processName + "!";
//        ins.printVarName = "";
//        return ins;
//    }
//
//    Instruction makeDeclare() {
//        Instruction ins;
//        ins.type = InstructionType::DECLARE;
//        ins.declareVar = randomVarName();
//        ins.declareValue = static_cast<uint16_t>(rng() % 100);
//        return ins;
//    }
//
//    Instruction makeArith(InstructionType opType) {
//        Instruction ins;
//        ins.type = opType;
//        ins.arithDest = randomVarName();
//        ins.arithSrc1 = Operand::fromLiteral(static_cast<uint16_t>(rng() % 50));
//        ins.arithSrc2 = Operand::fromLiteral(static_cast<uint16_t>(rng() % 50));
//        return ins;
//    }
//
//    Instruction makeSleep() {
//        Instruction ins;
//        ins.type = InstructionType::SLEEP;
//        ins.sleepTicks = static_cast<uint8_t>(1 + (rng() % 5));
//        return ins;
//    }*/
//
//std::vector<Instruction> generateRandom(const std::string& processName,
//    int count,
//    int depth,
//    int maxDepth)
//{
//    std::vector<Instruction> result;
//    for (int i = 0; i < count; ++i) {
//        if (i % 2 == 0) {
//            // Even index → PRINT("Value from: " + x)
//            result.push_back(makePrintX());
//        }
//        else {
//            // Odd index → ADD(x, x, [1-10])
//            result.push_back(makeAddX());
//        }
//    }
//    return result;
//}
//
//Instruction makePrintX() {
//    Instruction ins;
//    ins.type = InstructionType::PRINT;
//    ins.printLiteralMsg = "";       // no literal string
//    ins.printVarName = "x";        // print variable x
//    ins.printPrefix = "Value from: "; // prefix before the variable value
//    return ins;
//}
//
//Instruction makeAddX() {
//    Instruction ins;
//    ins.type = InstructionType::ADD;
//    ins.arithDest = "x";
//    ins.arithSrc1 = Operand::fromVar("x");                              // src1 = x
//    ins.arithSrc2 = Operand::fromLiteral(
//        static_cast<uint16_t>(1 + (rng() % 10)));  // src2 = random [1-10]
//    return ins;
//}
//
//case InstructionType::PRINT: {
//    if (!ins.printVarName.empty()) {
//        uint16_t val = process.symbolTable[ins.printVarName];
//        std::cout << ins.printPrefix << val << std::endl;
//    }
//    else {
//        std::cout << ins.printLiteralMsg << std::endl;
//    }
//    break;
//}
//
//    std::string randomVarName() {
//        static const char* names[] = { "x" };
//		return names[0]; // only "x"
//    }
//};

// InstructionGenerator.h
#pragma once
#include "Instruction.h"
#include <random>
#include <vector>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// GenerationMode controls what instruction set a process receives.
//
//   RANDOM   – original behaviour: random mix of PRINT/DECLARE/ADD/SUBTRACT/
//              SLEEP/FOR instructions.
//
//   XYZ_COUNTER – deterministic set required by the quiz test case:
//
//       FOR([
//           ADD  (x, x, 1),
//           PRINT("Value from: " +x),
//           ADD  (y, y, 1),
//           PRINT("Value from: " +y),
//           ADD  (z, z, 1),
//           PRINT("Value from: " +z)
//       ], 100)
//
//       Repeated enough times so the total flat instruction count falls in
//       [min-ins, max-ins].  (Each FOR expands to 100 * 6 = 600 flat
//       instructions, so we emit ceil(count / 600) FOR blocks.)
//
// NOTE: regardless of mode, the Process constructor must pre-populate x, y, z
//       in its symbol table with value 0.  See Process.h / Process.cpp.
// ─────────────────────────────────────────────────────────────────────────────

enum class GenerationMode {
    RANDOM,
    XYZ_COUNTER
};

class InstructionGenerator {
public:
    InstructionGenerator() : rng(std::random_device{}()) {}

    // ── Public interface ─────────────────────────────────────────────────────

    // Set the active mode before calling generate().
    void setMode(GenerationMode m) { mode = m; }
    GenerationMode getMode() const { return mode; }

    // Build the instruction list for a process with `count` instructions.
    // `depth` is used internally for recursive FOR generation (RANDOM mode).
    std::vector<Instruction> generate(const std::string& processName,
        int count,
        int depth = 0,
        int maxDepth = 3)
    {
        if (mode == GenerationMode::XYZ_COUNTER)
            return generateXYZ(count);
        else
            return generateRandom(processName, count, depth, maxDepth);
    }

private:
    std::mt19937   rng;
    GenerationMode mode = GenerationMode::RANDOM;

    // ── XYZ_COUNTER mode ────────────────────────────────────────────────────

    // Build the canonical FOR body:
    //   ADD(x, x, 1), PRINT("Value from: " +x),
    //   ADD(y, y, 1), PRINT("Value from: " +y),
    //   ADD(z, z, 1), PRINT("Value from: " +z)
    static std::vector<Instruction> makeXYZBody() {
        std::vector<Instruction> body;

        for (const char* var : { "x", "y", "z" }) {
            // ADD(var, var, 1)
            Instruction add;
            add.type = InstructionType::ADD;
            add.arithDest = var;
            add.arithSrc1 = Operand::fromVar(var);
            add.arithSrc2 = Operand::fromLiteral(1);
            body.push_back(add);

            // PRINT("Value from: " +var)
            Instruction pr;
            pr.type = InstructionType::PRINT;
            pr.printLiteralMsg = "Value from: ";
            pr.printVarName = var;   // executor appends the variable's value
            body.push_back(pr);
        }
        return body;
    }

    // One FOR block = 100 iterations × 6 body instructions = 600 flat instructions.
    static Instruction makeXYZFor() {
        Instruction f;
        f.type = InstructionType::FOR;
        f.forRepeats = 100;
        f.forBody = makeXYZBody();
        return f;
    }

    // Emit enough FOR blocks so flat count >= requested count.
    // Each FOR block contributes (forRepeats * bodySize) = 600 flat instructions.
    std::vector<Instruction> generateXYZ(int count) {
        const int flatPerBlock = 100 * 6; // 600
        int blocks = (count + flatPerBlock - 1) / flatPerBlock; // ceil division
        if (blocks < 1) blocks = 1;

        std::vector<Instruction> result;
        result.reserve(blocks);
        for (int i = 0; i < blocks; ++i)
            result.push_back(makeXYZFor());
        return result;
    }

    // ── RANDOM mode (original logic) ─────────────────────────────────────────

    std::vector<Instruction> generateRandom(const std::string& processName,
        int count,
        int depth,
        int maxDepth)
    {
        std::vector<Instruction> result;
        for (int i = 0; i < count; ++i) {
            int choice = rng() % 6;
            if (choice == 5 && depth < maxDepth) {
                Instruction forIns;
                forIns.type = InstructionType::FOR;
                forIns.forRepeats = 2 + (rng() % 3);
                int bodySize = 1 + (rng() % 3);
                forIns.forBody = generateRandom(processName, bodySize,
                    depth + 1, maxDepth);
                result.push_back(forIns);
            }
            else {
                switch (choice % 5) {
                case 0: result.push_back(makePrint(processName));              break;
                case 1: result.push_back(makeDeclare());                       break;
                case 2: result.push_back(makeArith(InstructionType::ADD));     break;
                case 3: result.push_back(makeArith(InstructionType::SUBTRACT)); break;
                case 4: result.push_back(makeSleep());                         break;
                }
            }
        }
        return result;
    }

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