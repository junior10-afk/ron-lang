#include "token.hpp"

namespace ron {

const char* tokenKindName(TokenKind kind) {
    switch (kind) {
    case TokenKind::EndOfFile:     return "end of file";
    case TokenKind::Error:         return "error";
    case TokenKind::Identifier:    return "identifier";
    case TokenKind::IntLiteral:    return "integer";
    case TokenKind::FloatLiteral:  return "float";
    case TokenKind::StringLiteral: return "string";
    case TokenKind::CharLiteral:   return "char";
    case TokenKind::KwFn:          return "fn";
    case TokenKind::KwReturn:      return "return";
    case TokenKind::KwIf:          return "if";
    case TokenKind::KwElse:        return "else";
    case TokenKind::KwWhile:       return "while";
    case TokenKind::KwFor:         return "for";
    case TokenKind::KwBreak:       return "break";
    case TokenKind::KwContinue:    return "continue";
    case TokenKind::KwStruct:      return "struct";
    case TokenKind::KwEnum:        return "enum";
    case TokenKind::KwConst:       return "const";
    case TokenKind::KwExtern:      return "extern";
    case TokenKind::KwImport:      return "import";
    case TokenKind::KwAs:          return "as";
    case TokenKind::KwSizeof:      return "sizeof";
    case TokenKind::KwTrue:        return "true";
    case TokenKind::KwFalse:       return "false";
    case TokenKind::KwNull:        return "null";
    case TokenKind::KwVar:         return "var";
    case TokenKind::KwSwitch:      return "switch";
    case TokenKind::KwCase:        return "case";
    case TokenKind::KwDefault:     return "default";
    case TokenKind::KwDefer:       return "defer";
    case TokenKind::KwDo:          return "do";
    case TokenKind::KwMatch:       return "match";
    case TokenKind::KwIn:          return "in";
    case TokenKind::KwPrivate:     return "private";
    case TokenKind::DotDot:        return "..";
    case TokenKind::FatArrow:      return "=>";
    case TokenKind::LParen:        return "(";
    case TokenKind::RParen:        return ")";
    case TokenKind::LBrace:        return "{";
    case TokenKind::RBrace:        return "}";
    case TokenKind::LBracket:      return "[";
    case TokenKind::RBracket:      return "]";
    case TokenKind::Comma:         return ",";
    case TokenKind::Semicolon:     return ";";
    case TokenKind::Colon:         return ":";
    case TokenKind::ColonColon:    return "::";
    case TokenKind::Dot:           return ".";
    case TokenKind::Arrow:         return "->";
    case TokenKind::Question:      return "?";
    case TokenKind::At:            return "@";
    case TokenKind::Plus:          return "+";
    case TokenKind::Minus:         return "-";
    case TokenKind::Star:          return "*";
    case TokenKind::Slash:         return "/";
    case TokenKind::Percent:       return "%";
    case TokenKind::Amp:           return "&";
    case TokenKind::Pipe:          return "|";
    case TokenKind::Caret:         return "^";
    case TokenKind::Tilde:         return "~";
    case TokenKind::Bang:          return "!";
    case TokenKind::Shl:           return "<<";
    case TokenKind::Shr:           return ">>";
    case TokenKind::AmpAmp:        return "&&";
    case TokenKind::PipePipe:      return "||";
    case TokenKind::PlusPlus:      return "++";
    case TokenKind::MinusMinus:    return "--";
    case TokenKind::Eq:            return "=";
    case TokenKind::EqEq:          return "==";
    case TokenKind::BangEq:        return "!=";
    case TokenKind::Less:          return "<";
    case TokenKind::LessEq:        return "<=";
    case TokenKind::Greater:       return ">";
    case TokenKind::GreaterEq:     return ">=";
    case TokenKind::PlusEq:        return "+=";
    case TokenKind::MinusEq:       return "-=";
    case TokenKind::StarEq:        return "*=";
    case TokenKind::SlashEq:       return "/=";
    case TokenKind::PercentEq:     return "%=";
    case TokenKind::AmpEq:         return "&=";
    case TokenKind::PipeEq:        return "|=";
    case TokenKind::CaretEq:       return "^=";
    case TokenKind::ShlEq:         return "<<=";
    case TokenKind::ShrEq:         return ">>=";
    }
    return "?";
}

} // namespace ron
