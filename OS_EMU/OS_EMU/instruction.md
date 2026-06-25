// this is instruction.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

enum class InstructionType {
    PRINT,
    DECLARE,
    ADD,
    SUBTRACT,
    SLEEP,
    FOR
};

// An operand is either a literal uint16 value or a variable name.
// isLiteral tells you which one to use.
struct Operand {
    bool isLiteral;
    uint16_t literalValue;
    std::string varName;

    Operand() : isLiteral(true), literalValue(0) {}
    static Operand fromLiteral(uint16_t v) {
        Operand o; o.isLiteral = true; o.literalValue = v; return o;
    }
    static Operand fromVar(const std::string& name) {
        Operand o; o.isLiteral = false; o.varName = name; return o;
    }
};

struct Instruction {
    InstructionType type;

    // PRINT: optional variable to append to the message ("" means none)
    std::string printVarName;
    std::string printLiteralMsg; // used when no variable / default message

    // DECLARE: var, value
    std::string declareVar;
    uint16_t declareValue = 0;

    // ADD / SUBTRACT: var1 = var2 op var3
    std::string arithDest;
    Operand arithSrc1;
    Operand arithSrc2;

    // SLEEP: ticks to sleep
    uint8_t sleepTicks = 0;

    // FOR: nested instruction list + repeat count
    std::vector<Instruction> forBody;
    int forRepeats = 0;
};
