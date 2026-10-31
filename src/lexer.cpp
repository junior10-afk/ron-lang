#include "lexer.hpp"

#include <charconv>
#include <limits>

namespace ron {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHexDigit(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
// Bytes >= 0x80 (UTF-8) are accepted so identifiers may contain accents: `énergie`.
bool isIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}
bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

int hexValue(char c) {
    if (isDigit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

struct Keyword {
    std::string_view text;
    TokenKind kind;
};

constexpr Keyword kKeywords[] = {
    {"fn", TokenKind::KwFn},           {"return", TokenKind::KwReturn},
    {"if", TokenKind::KwIf},           {"else", TokenKind::KwElse},
    {"while", TokenKind::KwWhile},     {"for", TokenKind::KwFor},
    {"break", TokenKind::KwBreak},     {"continue", TokenKind::KwContinue},
    {"struct", TokenKind::KwStruct},   {"enum", TokenKind::KwEnum},
    {"const", TokenKind::KwConst},     {"extern", TokenKind::KwExtern},
    {"import", TokenKind::KwImport},   {"as", TokenKind::KwAs},
    {"sizeof", TokenKind::KwSizeof},
    {"true", TokenKind::KwTrue},       {"false", TokenKind::KwFalse},
    {"null", TokenKind::KwNull},
};

} // namespace

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

Lexer::Lexer(const SourceFile& file, uint32_t fileId, Diagnostics& diags)
    : src_(file.text), diags_(diags) {
    loc_.fileId = fileId;
    // Skip a UTF-8 byte order mark.
    if (src_.size() >= 3 && src_.substr(0, 3) == "\xEF\xBB\xBF") {
        pos_ = 3;
        loc_.offset = 3;
    }
}

char Lexer::peek(size_t ahead) const {
    return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
}

char Lexer::advance() {
    char c = src_[pos_++];
    loc_.offset = static_cast<uint32_t>(pos_);
    if (c == '\n') {
        ++loc_.line;
        loc_.column = 1;
    } else {
        ++loc_.column;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (atEnd() || src_[pos_] != expected) return false;
    advance();
    return true;
}

void Lexer::skipTrivia() {
    while (!atEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            while (!atEnd() && peek() != '\n') advance();
        } else if (c == '/' && peek(1) == '*') {
            // Block comments nest: /* a /* b */ c */
            SourceLoc start = loc_;
            advance();
            advance();
            int depth = 1;
            while (!atEnd() && depth > 0) {
                if (peek() == '/' && peek(1) == '*') {
                    advance(); advance(); ++depth;
                } else if (peek() == '*' && peek(1) == '/') {
                    advance(); advance(); --depth;
                } else {
                    advance();
                }
            }
            if (depth > 0) diags_.error(start, "unterminated /* comment");
        } else {
            break;
        }
    }
}

Token Lexer::make(TokenKind kind, SourceLoc start) const {
    Token t;
    t.kind = kind;
    t.loc = start;
    t.text = src_.substr(start.offset, pos_ - start.offset);
    return t;
}

Token Lexer::error(SourceLoc start, SourceLoc at, std::string message) {
    diags_.error(at, std::move(message));
    return make(TokenKind::Error, start);
}

Token Lexer::next() {
    skipTrivia();
    SourceLoc start = loc_;
    if (atEnd()) return make(TokenKind::EndOfFile, start);

    char c = advance();

    if (isIdentStart(c)) return lexIdentifier(start);
    if (isDigit(c)) return lexNumber(start);

    switch (c) {
    case '"': return lexString(start);
    case '\'': return lexChar(start);
    case '(': return make(TokenKind::LParen, start);
    case ')': return make(TokenKind::RParen, start);
    case '{': return make(TokenKind::LBrace, start);
    case '}': return make(TokenKind::RBrace, start);
    case '[': return make(TokenKind::LBracket, start);
    case ']': return make(TokenKind::RBracket, start);
    case ',': return make(TokenKind::Comma, start);
    case ';': return make(TokenKind::Semicolon, start);
    case '?': return make(TokenKind::Question, start);
    case '@': return make(TokenKind::At, start);
    case '~': return make(TokenKind::Tilde, start);
    case '.': return make(TokenKind::Dot, start);
    case ':': return make(match(':') ? TokenKind::ColonColon : TokenKind::Colon, start);
    case '+':
        if (match('+')) return make(TokenKind::PlusPlus, start);
        return make(match('=') ? TokenKind::PlusEq : TokenKind::Plus, start);
    case '-':
        if (match('-')) return make(TokenKind::MinusMinus, start);
        if (match('>')) return make(TokenKind::Arrow, start);
        return make(match('=') ? TokenKind::MinusEq : TokenKind::Minus, start);
    case '*': return make(match('=') ? TokenKind::StarEq : TokenKind::Star, start);
    case '/': return make(match('=') ? TokenKind::SlashEq : TokenKind::Slash, start);
    case '%': return make(match('=') ? TokenKind::PercentEq : TokenKind::Percent, start);
    case '^': return make(match('=') ? TokenKind::CaretEq : TokenKind::Caret, start);
    case '=': return make(match('=') ? TokenKind::EqEq : TokenKind::Eq, start);
    case '!': return make(match('=') ? TokenKind::BangEq : TokenKind::Bang, start);
    case '&':
        if (match('&')) return make(TokenKind::AmpAmp, start);
        return make(match('=') ? TokenKind::AmpEq : TokenKind::Amp, start);
    case '|':
        if (match('|')) return make(TokenKind::PipePipe, start);
        return make(match('=') ? TokenKind::PipeEq : TokenKind::Pipe, start);
    case '<':
        if (match('<')) return make(match('=') ? TokenKind::ShlEq : TokenKind::Shl, start);
        return make(match('=') ? TokenKind::LessEq : TokenKind::Less, start);
    case '>':
        if (match('>')) return make(match('=') ? TokenKind::ShrEq : TokenKind::Shr, start);
        return make(match('=') ? TokenKind::GreaterEq : TokenKind::Greater, start);
    default:
        break;
    }

    std::string shown = (c >= 0x20 && c < 0x7F) ? std::string("'") + c + "'"
                                                : "byte " + std::to_string(static_cast<unsigned char>(c));
    return error(start, start, "unexpected character " + shown);
}

Token Lexer::lexIdentifier(SourceLoc start) {
    while (isIdentChar(peek())) advance();
    Token t = make(TokenKind::Identifier, start);
    for (const auto& kw : kKeywords) {
        if (kw.text == t.text) {
            t.kind = kw.kind;
            break;
        }
    }
    return t;
}

Token Lexer::lexNumber(SourceLoc start) {
    char first = src_[start.offset];

    // Hexadecimal (0xFF) or binary (0b1010), `_` separators allowed.
    if (first == '0' && (peek() == 'x' || peek() == 'X' || peek() == 'b' || peek() == 'B')) {
        bool hex = (advance() | 0x20) == 'x';
        unsigned base = hex ? 16 : 2;
        uint64_t value = 0;
        bool overflow = false;
        size_t digits = 0;
        while (true) {
            char c = peek();
            if (c == '_') { advance(); continue; }
            if (hex ? !isHexDigit(c) : (c != '0' && c != '1')) break;
            advance();
            unsigned d = static_cast<unsigned>(hexValue(c));
            if (value > (std::numeric_limits<uint64_t>::max() - d) / base) overflow = true;
            value = value * base + d;
            ++digits;
        }
        if (digits == 0)
            return error(start, loc_, hex ? "expected hexadecimal digits after 0x"
                                          : "expected binary digits after 0b");
        if (isIdentChar(peek()))
            return error(start, loc_, "invalid character in number literal");
        if (overflow) return error(start, start, "integer literal too large (64 bits max)");
        Token t = make(TokenKind::IntLiteral, start);
        t.intValue = value;
        return t;
    }

    // Decimal: integer or float.
    std::string clean(1, first);
    auto eatDigits = [&] {
        while (isDigit(peek()) || peek() == '_') {
            char c = advance();
            if (c != '_') clean += c;
        }
    };
    eatDigits();

    bool isFloat = false;
    // `1.5` is a float, but `1.foo` is the integer 1 followed by `.foo`.
    if (peek() == '.' && isDigit(peek(1))) {
        isFloat = true;
        clean += advance();
        eatDigits();
    }
    if (peek() == 'e' || peek() == 'E') {
        size_t signLen = (peek(1) == '+' || peek(1) == '-') ? 1 : 0;
        if (isDigit(peek(1 + signLen))) {
            isFloat = true;
            clean += advance();
            if (signLen) clean += advance();
            eatDigits();
        }
    }

    bool f32 = false;
    if (peek() == 'f' && !isIdentChar(peek(1))) {
        advance();
        isFloat = true;
        f32 = true;
    }
    if (isIdentChar(peek())) return error(start, loc_, "invalid character in number literal");

    Token t = make(isFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral, start);
    const char* b = clean.data();
    const char* e = b + clean.size();
    if (isFloat) {
        auto [ptr, ec] = std::from_chars(b, e, t.floatValue);
        if (ec != std::errc() || ptr != e) return error(start, start, "invalid float literal");
        t.isF32 = f32;
    } else {
        auto [ptr, ec] = std::from_chars(b, e, t.intValue);
        if (ec == std::errc::result_out_of_range)
            return error(start, start, "integer literal too large (64 bits max)");
        if (ec != std::errc() || ptr != e) return error(start, start, "invalid integer literal");
    }
    return t;
}

bool Lexer::lexEscape(std::string& out) {
    SourceLoc at = loc_;
    if (atEnd()) return false;
    char c = advance();
    switch (c) {
    case 'n': out += '\n'; return true;
    case 't': out += '\t'; return true;
    case 'r': out += '\r'; return true;
    case '0': out += '\0'; return true;
    case '\\': out += '\\'; return true;
    case '"': out += '"'; return true;
    case '\'': out += '\''; return true;
    case 'x': {
        // \xHH: a single byte
        if (!isHexDigit(peek()) || !isHexDigit(peek(1))) {
            diags_.error(at, "\\x must be followed by 2 hexadecimal digits");
            return false;
        }
        int hi = hexValue(advance());
        int lo = hexValue(advance());
        out += static_cast<char>(hi * 16 + lo);
        return true;
    }
    case 'u': {
        // \u{1F600}: a Unicode code point, encoded as UTF-8
        if (!match('{')) {
            diags_.error(at, "\\u must be followed by {code}");
            return false;
        }
        uint32_t cp = 0;
        int digits = 0;
        while (isHexDigit(peek()) && digits < 6) {
            cp = cp * 16 + static_cast<uint32_t>(hexValue(advance()));
            ++digits;
        }
        if (digits == 0 || !match('}') || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            diags_.error(at, "invalid \\u{...} escape");
            return false;
        }
        appendUtf8(out, cp);
        return true;
    }
    default:
        diags_.error(at, std::string("unknown escape sequence \\") + c);
        return false;
    }
}

Token Lexer::lexString(SourceLoc start) {
    std::string value;
    bool ok = true;
    while (true) {
        if (atEnd() || peek() == '\n') {
            return error(start, start, "unterminated string literal (missing closing \")");
        }
        char c = advance();
        if (c == '"') break;
        if (c == '\\') {
            ok = lexEscape(value) && ok;
        } else {
            value += c;
        }
    }
    Token t = make(ok ? TokenKind::StringLiteral : TokenKind::Error, start);
    t.strValue = std::move(value);
    return t;
}

bool Lexer::decodeUtf8(uint32_t& cp) {
    auto b0 = static_cast<unsigned char>(advance());
    int extra = b0 < 0x80 ? 0 : (b0 >> 5) == 0x6 ? 1 : (b0 >> 4) == 0xE ? 2 : (b0 >> 3) == 0x1E ? 3 : -1;
    if (extra < 0) return false;
    cp = extra == 0 ? b0 : b0 & (0x3F >> extra);
    for (int i = 0; i < extra; ++i) {
        auto b = static_cast<unsigned char>(peek());
        if ((b & 0xC0) != 0x80) return false;
        advance();
        cp = (cp << 6) | (b & 0x3F);
    }
    return true;
}

Token Lexer::lexChar(SourceLoc start) {
    // 'a', '\n', 'é' → integer value of the code point
    if (atEnd() || peek() == '\'' || peek() == '\n') {
        match('\'');
        return error(start, start, "empty character literal ''");
    }

    uint32_t cp = 0;
    if (peek() == '\\') {
        advance();
        std::string buf;
        if (!lexEscape(buf)) {
            while (!atEnd() && peek() != '\'' && peek() != '\n') advance();
            match('\'');
            return make(TokenKind::Error, start);
        }
        // Decode the escape result again (it may be UTF-8 for \u{...}).
        auto b0 = static_cast<unsigned char>(buf[0]);
        if (buf.size() == 1) {
            cp = b0;
        } else {
            int extra = static_cast<int>(buf.size()) - 1;
            cp = b0 & (0x3F >> extra);
            for (size_t i = 1; i < buf.size(); ++i) cp = (cp << 6) | (buf[i] & 0x3F);
        }
    } else if (!decodeUtf8(cp)) {
        return error(start, start, "invalid UTF-8 in character literal");
    }

    if (!match('\'')) {
        while (!atEnd() && peek() != '\'' && peek() != '\n') advance();
        match('\'');
        return error(start, start, "a character literal holds a single character (use \"...\" for strings)");
    }
    Token t = make(TokenKind::CharLiteral, start);
    t.intValue = cp;
    return t;
}

std::vector<Token> Lexer::tokenizeAll() {
    std::vector<Token> tokens;
    while (true) {
        tokens.push_back(next());
        if (tokens.back().is(TokenKind::EndOfFile)) break;
    }
    return tokens;
}

} // namespace ron
