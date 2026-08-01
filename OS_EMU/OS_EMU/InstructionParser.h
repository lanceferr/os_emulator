// InstructionParser.h
#pragma once
#include "Instruction.h"
#include <vector>
#include <string>
#include <sstream>
#include <cctype>

// Parses the semicolon-separated instruction string used by "screen -c",
// e.g.:
//   DECLARE varA 10; DECLARE varB 5; ADD varA varA varB; WRITE 0x500 varA;
//   READ varC 0x500; PRINT("Result: " + varC)
//
// Supported statements: DECLARE, ADD, SUBTRACT, SLEEP, READ, WRITE, PRINT.
// FOR loops are not supported by this parser (the spec's screen -c examples
// don't use them) — random/nested-FOR programs still come from
// InstructionGenerator as before.
class InstructionParser {
public:
    // Returns true and fills 'out' on success. On failure returns false and
    // fills 'error' with a short human-readable reason.
    static bool parse(const std::string& text, std::vector<Instruction>& out, std::string& error) {
        for (auto& stmt : splitStatements(text)) {
            std::string s = trim(stmt);
            if (s.empty()) continue;
            Instruction ins;
            if (!parseStatement(s, ins, error)) return false;
            out.push_back(ins);
        }
        if (out.empty()) {
            error = "no instructions found";
            return false;
        }
        return true;
    }

private:
    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // Splits on ';' but ignores ';' that appears inside a double-quoted string.
    static std::vector<std::string> splitStatements(const std::string& text) {
        std::vector<std::string> result;
        std::string cur;
        bool inQuotes = false;
        for (char c : text) {
            if (c == '"') inQuotes = !inQuotes;
            if (c == ';' && !inQuotes) {
                result.push_back(cur);
                cur.clear();
            }
            else {
                cur += c;
            }
        }
        if (!trim(cur).empty()) result.push_back(cur);
        return result;
    }

    static std::vector<std::string> splitWs(const std::string& s) {
        std::istringstream iss(s);
        std::vector<std::string> toks;
        std::string t;
        while (iss >> t) toks.push_back(t);
        return toks;
    }

    static bool isNumber(const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) if (!isdigit((unsigned char)c)) return false;
        return true;
    }

    static Operand parseOperand(const std::string& tok) {
        if (isNumber(tok)) return Operand::fromLiteral(static_cast<uint16_t>(std::stoul(tok)));
        return Operand::fromVar(tok);
    }

    static bool parseHexAddress(const std::string& tok, uint32_t& out) {
        if (tok.size() < 3 || tok[0] != '0' || (tok[1] != 'x' && tok[1] != 'X')) return false;
        try {
            out = static_cast<uint32_t>(std::stoul(tok.substr(2), nullptr, 16));
        }
        catch (...) { return false; }
        return true;
    }

    static std::string toUpper(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) r += static_cast<char>(toupper((unsigned char)c));
        return r;
    }

    static bool parseStatement(const std::string& s, Instruction& ins, std::string& error) {
        std::string upper = toUpper(s);
        if (upper.rfind("PRINT", 0) == 0) {
            return parsePrint(s, ins, error);
        }

        auto toks = splitWs(s);
        if (toks.empty()) { error = "empty instruction"; return false; }
        std::string cmd = toUpper(toks[0]);

        if (cmd == "DECLARE") {
            if (toks.size() != 3) { error = "DECLARE requires <var> <value>"; return false; }
            ins.type = InstructionType::DECLARE;
            ins.declareVar = toks[1];
            ins.declareValue = static_cast<uint16_t>(std::stoul(toks[2]));
            return true;
        }
        if (cmd == "ADD" || cmd == "SUBTRACT") {
            if (toks.size() != 4) { error = cmd + " requires <dest> <src1> <src2>"; return false; }
            ins.type = (cmd == "ADD") ? InstructionType::ADD : InstructionType::SUBTRACT;
            ins.arithDest = toks[1];
            ins.arithSrc1 = parseOperand(toks[2]);
            ins.arithSrc2 = parseOperand(toks[3]);
            return true;
        }
        if (cmd == "READ") {
            if (toks.size() != 3) { error = "READ requires <var> <memory_address>"; return false; }
            uint32_t addr;
            if (!parseHexAddress(toks[2], addr)) { error = "invalid hex address in READ"; return false; }
            ins.type = InstructionType::READ;
            ins.readVar = toks[1];
            ins.readAddress = addr;
            return true;
        }
        if (cmd == "WRITE") {
            if (toks.size() != 3) { error = "WRITE requires <memory_address> <value>"; return false; }
            uint32_t addr;
            if (!parseHexAddress(toks[1], addr)) { error = "invalid hex address in WRITE"; return false; }
            ins.type = InstructionType::WRITE;
            ins.writeAddress = addr;
            ins.writeValue = parseOperand(toks[2]);
            return true;
        }
        if (cmd == "SLEEP") {
            if (toks.size() != 2) { error = "SLEEP requires <ticks>"; return false; }
            ins.type = InstructionType::SLEEP;
            ins.sleepTicks = static_cast<uint8_t>(std::stoul(toks[1]));
            return true;
        }

        error = "unrecognized instruction: " + toks[0];
        return false;
    }

    // Handles: PRINT("literal"), PRINT("literal" + var), PRINT(var)
    static bool parsePrint(const std::string& s, Instruction& ins, std::string& error) {
        size_t open = s.find('(');
        size_t close = s.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close <= open) {
            error = "malformed PRINT statement";
            return false;
        }
        std::string inner = trim(s.substr(open + 1, close - open - 1));

        ins.type = InstructionType::PRINT;
        ins.printLiteralMsg = "";
        ins.printVarName = "";

        size_t q1 = inner.find('"');
        if (q1 == std::string::npos) {
            // No literal string: PRINT(var)
            ins.printVarName = trim(inner);
            return true;
        }
        size_t q2 = inner.find('"', q1 + 1);
        if (q2 == std::string::npos) { error = "unterminated string in PRINT"; return false; }
        ins.printLiteralMsg = inner.substr(q1 + 1, q2 - q1 - 1);

        std::string rest = trim(inner.substr(q2 + 1));
        if (!rest.empty() && rest[0] == '+') {
            ins.printVarName = trim(rest.substr(1));
        }
        return true;
    }
};
