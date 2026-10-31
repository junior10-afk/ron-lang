#include "ast.hpp"

namespace ron {

const char* binaryOpText(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Mod: return "%";
    case BinaryOp::BitAnd: return "&";
    case BinaryOp::BitOr: return "|";
    case BinaryOp::BitXor: return "^";
    case BinaryOp::Shl: return "<<";
    case BinaryOp::Shr: return ">>";
    case BinaryOp::Eq: return "==";
    case BinaryOp::Ne: return "!=";
    case BinaryOp::Lt: return "<";
    case BinaryOp::Le: return "<=";
    case BinaryOp::Gt: return ">";
    case BinaryOp::Ge: return ">=";
    case BinaryOp::LogAnd: return "&&";
    case BinaryOp::LogOr: return "||";
    }
    return "?";
}

const char* unaryOpText(UnaryOp op) {
    switch (op) {
    case UnaryOp::Neg: return "-";
    case UnaryOp::Not: return "!";
    case UnaryOp::BitNot: return "~";
    case UnaryOp::AddrOf: return "&";
    case UnaryOp::Deref: return "*";
    case UnaryOp::PreInc:
    case UnaryOp::PostInc: return "++";
    case UnaryOp::PreDec:
    case UnaryOp::PostDec: return "--";
    }
    return "?";
}

} // namespace ron
