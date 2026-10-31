#include "test.hpp"

#include "lexer.hpp"

using namespace ron;

namespace {

struct Lexed {
    std::vector<Token> tokens;
    size_t errors = 0;
    std::string firstMessage;
};

// Keeps sources alive for the whole test run (tokens point into them).
SourceManager& testSources() {
    static SourceManager s;
    return s;
}

Lexed lex(std::string src) {
    Lexed r;
    uint32_t id = testSources().add("test.ron", std::move(src));
    Diagnostics diags(testSources());
    Lexer lexer(testSources().get(id), id, diags);
    r.tokens = lexer.tokenizeAll();
    r.errors = diags.errorCount();
    if (!diags.all().empty()) r.firstMessage = diags.format(diags.all()[0]);
    return r;
}

std::vector<TokenKind> kinds(const Lexed& l) {
    std::vector<TokenKind> out;
    for (const auto& t : l.tokens) out.push_back(t.kind);
    return out;
}

} // namespace

TEST(lexer_empty_file) {
    auto l = lex("");
    CHECK_EQ(l.tokens.size(), 1u);
    CHECK(l.tokens[0].is(TokenKind::EndOfFile));
    CHECK_EQ(l.errors, 0u);
}

TEST(lexer_keywords_and_identifiers) {
    auto l = lex("fn main return foo_bar _x struct énergie sizeof");
    std::vector<TokenKind> expected = {
        TokenKind::KwFn, TokenKind::Identifier, TokenKind::KwReturn, TokenKind::Identifier,
        TokenKind::Identifier, TokenKind::KwStruct, TokenKind::Identifier, TokenKind::KwSizeof,
        TokenKind::EndOfFile};
    CHECK(kinds(l) == expected);
    CHECK_EQ(l.tokens[1].text, "main");
    CHECK_EQ(l.tokens[6].text, "énergie");
    CHECK_EQ(l.errors, 0u);
}

TEST(lexer_integers) {
    auto l = lex("0 42 1_000_000 0xFF 0b1010 18446744073709551615");
    CHECK_EQ(l.errors, 0u);
    CHECK_EQ(l.tokens[0].intValue, 0u);
    CHECK_EQ(l.tokens[1].intValue, 42u);
    CHECK_EQ(l.tokens[2].intValue, 1000000u);
    CHECK_EQ(l.tokens[3].intValue, 255u);
    CHECK_EQ(l.tokens[4].intValue, 10u);
    CHECK_EQ(l.tokens[5].intValue, 18446744073709551615ull);
}

TEST(lexer_integer_too_large) {
    CHECK_EQ(lex("18446744073709551616").errors, 1u);
    CHECK_EQ(lex("0x1_0000_0000_0000_0000").errors, 1u);
}

TEST(lexer_floats) {
    auto l = lex("1.5 0.25f 3e2 1.5e-3 2f");
    CHECK_EQ(l.errors, 0u);
    for (int i = 0; i < 5; ++i) CHECK(l.tokens[i].is(TokenKind::FloatLiteral));
    CHECK_EQ(l.tokens[0].floatValue, 1.5);
    CHECK_EQ(l.tokens[1].floatValue, 0.25);
    CHECK(l.tokens[1].isF32);
    CHECK(!l.tokens[0].isF32);
    CHECK_EQ(l.tokens[2].floatValue, 300.0);
    CHECK_EQ(l.tokens[3].floatValue, 0.0015);
    CHECK_EQ(l.tokens[4].floatValue, 2.0);
}

TEST(lexer_dot_after_integer_is_member_access) {
    auto l = lex("v.x 1.len");
    std::vector<TokenKind> expected = {
        TokenKind::Identifier, TokenKind::Dot, TokenKind::Identifier,
        TokenKind::IntLiteral, TokenKind::Dot, TokenKind::Identifier, TokenKind::EndOfFile};
    CHECK(kinds(l) == expected);
}

TEST(lexer_malformed_numbers) {
    CHECK_EQ(lex("12abc").errors, 1u);
    CHECK_EQ(lex("0x").errors, 1u);
    CHECK_EQ(lex("1e").errors, 1u);
}

TEST(lexer_strings_and_escapes) {
    auto l = lex(R"("hello\n" "a\tb" "\x41\u{E9}" "")");
    CHECK_EQ(l.errors, 0u);
    CHECK_EQ(l.tokens[0].strValue, "hello\n");
    CHECK_EQ(l.tokens[1].strValue, "a\tb");
    CHECK_EQ(l.tokens[2].strValue, "A\xC3\xA9");
    CHECK_EQ(l.tokens[3].strValue, "");
}

TEST(lexer_unterminated_string_recovers) {
    auto l = lex("\"oops\nfn");
    CHECK_EQ(l.errors, 1u);
    CHECK(l.tokens[0].is(TokenKind::Error));
    CHECK(l.tokens[1].is(TokenKind::KwFn));
}

TEST(lexer_char_literals) {
    auto l = lex(R"('a' '\n' 'é' '\u{1F600}')");
    CHECK_EQ(l.errors, 0u);
    CHECK_EQ(l.tokens[0].intValue, 97u);
    CHECK_EQ(l.tokens[1].intValue, 10u);
    CHECK_EQ(l.tokens[2].intValue, 0xE9u);
    CHECK_EQ(l.tokens[3].intValue, 0x1F600u);
    CHECK_EQ(lex("'ab'").errors, 1u);
    CHECK_EQ(lex("''").errors, 1u);
}

TEST(lexer_operators) {
    auto l = lex("+ += ++ - -= -- -> << <<= >> >>= <= >= == != && || :: : . ! ~ ?");
    std::vector<TokenKind> expected = {
        TokenKind::Plus, TokenKind::PlusEq, TokenKind::PlusPlus,
        TokenKind::Minus, TokenKind::MinusEq, TokenKind::MinusMinus, TokenKind::Arrow,
        TokenKind::Shl, TokenKind::ShlEq, TokenKind::Shr, TokenKind::ShrEq,
        TokenKind::LessEq, TokenKind::GreaterEq, TokenKind::EqEq, TokenKind::BangEq,
        TokenKind::AmpAmp, TokenKind::PipePipe, TokenKind::ColonColon, TokenKind::Colon,
        TokenKind::Dot, TokenKind::Bang, TokenKind::Tilde, TokenKind::Question,
        TokenKind::EndOfFile};
    CHECK(kinds(l) == expected);
    CHECK_EQ(l.errors, 0u);
}

TEST(lexer_comments) {
    auto l = lex("a // end of line\n/* block /* nested */ still */ b");
    std::vector<TokenKind> expected = {TokenKind::Identifier, TokenKind::Identifier, TokenKind::EndOfFile};
    CHECK(kinds(l) == expected);
    CHECK_EQ(l.errors, 0u);
    CHECK_EQ(lex("/* never closed").errors, 1u);
}

TEST(lexer_line_and_column) {
    auto l = lex("fn\n  main");
    CHECK_EQ(l.tokens[0].loc.line, 1u);
    CHECK_EQ(l.tokens[0].loc.column, 1u);
    CHECK_EQ(l.tokens[1].loc.line, 2u);
    CHECK_EQ(l.tokens[1].loc.column, 3u);
}

TEST(lexer_unexpected_character) {
    auto l = lex("a $ b");
    CHECK_EQ(l.errors, 1u);
    CHECK(l.tokens[1].is(TokenKind::Error));
    CHECK(l.tokens[2].is(TokenKind::Identifier));
}

TEST(lexer_diagnostic_format) {
    auto l = lex("int x = $;");
    CHECK_EQ(l.errors, 1u);
    CHECK(l.firstMessage.find("test.ron:1:9: error:") == 0);
    CHECK(l.firstMessage.find("            ^") != std::string::npos);
}
