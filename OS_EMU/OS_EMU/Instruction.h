//instruction.h
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

    // PRINT
    std::string printVarName;      // variable to append (empty = none)
    std::string printLiteralMsg;   // base message text

    // DECLARE
    std::string declareVar;
    uint16_t    declareValue = 0;

    // ADD / SUBTRACT: dest = src1 op src2
    std::string arithDest;
    Operand     arithSrc1;
    Operand     arithSrc2;

    // SLEEP
    uint8_t sleepTicks = 0;

    // FOR
    std::vector<Instruction> forBody;
    int forRepeats = 0;
};