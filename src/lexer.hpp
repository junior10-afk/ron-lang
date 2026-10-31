#pragma once

#include "diagnostics.hpp"
#include "token.hpp"

#include <vector>

namespace ron {

// Turns source text into a sequence of tokens.
// Errors are reported to Diagnostics; the lexer keeps going after an error so a
// single pass reports as many problems as possible.
class Lexer {
public:
    Lexer(const SourceFile& file, uint32_t fileId, Diagnostics& diags);

    Token next();
    std::vector<Token> tokenizeAll();   // always ends with EndOfFile

private:
    char peek(size_t ahead = 0) const;
    char advance();
    bool match(char expected);
    bool atEnd() const { return pos_ >= src_.size(); }

    void skipTrivia();
    Token make(TokenKind kind, SourceLoc start) const;
    Token error(SourceLoc start, SourceLoc at, std::string message);

    Token lexIdentifier(SourceLoc start);
    Token lexNumber(SourceLoc start);
    Token lexString(SourceLoc start);
    Token lexChar(SourceLoc start);
    bool lexEscape(std::string& out);   // called after the '\'
    bool decodeUtf8(uint32_t& codepoint);

    std::string_view src_;
    Diagnostics& diags_;
    size_t pos_ = 0;
    SourceLoc loc_;
};

void appendUtf8(std::string& out, uint32_t codepoint);

} // namespace ron
