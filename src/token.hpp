#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ron {

// Position in a source file (line and column start at 1).
struct SourceLoc {
    uint32_t offset = 0;
    uint32_t line = 1;
    uint32_t column = 1;
    uint32_t fileId = 0;
};

enum class TokenKind : uint8_t {
    // Special
    EndOfFile,
    Error,

    // Literals and names
    Identifier,
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    CharLiteral,

    // Keywords
    KwFn, KwReturn, KwIf, KwElse, KwWhile, KwFor, KwBreak, KwContinue,
    KwStruct, KwEnum, KwConst, KwExtern, KwImport, KwAs, KwSizeof,
    KwTrue, KwFalse, KwNull,

    // Delimiters
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semicolon, Colon, ColonColon, Dot, Arrow, Question, At,

    // Operators
    Plus, Minus, Star, Slash, Percent,
    Amp, Pipe, Caret, Tilde, Bang,
    Shl, Shr,
    AmpAmp, PipePipe,
    PlusPlus, MinusMinus,
    Eq, EqEq, BangEq, Less, LessEq, Greater, GreaterEq,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    AmpEq, PipeEq, CaretEq, ShlEq, ShrEq,
};

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string_view text;   // slice of the source text (no copy)
    SourceLoc loc;

    // Decoded literal values
    uint64_t intValue = 0;
    double floatValue = 0.0;
    bool isF32 = false;      // `f` suffix on a float: 1.5f
    std::string strValue;    // string contents after escape processing

    bool is(TokenKind k) const { return kind == k; }
};

// Human-readable token kind, used in messages and token dumps.
const char* tokenKindName(TokenKind kind);

} // namespace ron
