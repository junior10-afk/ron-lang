#pragma once

#include "ast.hpp"
#include "diagnostics.hpp"

#include <vector>

namespace ron {

// Recursive-descent parser. Expressions use precedence climbing.
// On a syntax error it reports a diagnostic and resynchronizes at the next
// statement or declaration, so one run can report several errors.
class Parser {
public:
    Parser(std::vector<Token> tokens, AstArena& arena, Diagnostics& diags);

    // Appends the declarations of this file to `program`.
    void parseProgram(Program& program);

    // Exposed for tests.
    Expr* parseExpression();

private:
    struct ParseError {};

    // Token helpers
    const Token& peek(size_t ahead = 0) const;
    const Token& advance();
    bool check(TokenKind k) const { return peek().is(k); }
    bool match(TokenKind k);
    const Token& expect(TokenKind k, const char* what);
    [[noreturn]] void fail(const Token& at, const std::string& message);

    // Declarations
    Decl* parseDecl();
    FuncDecl* parseFunc(const std::string& externDll);
    void parseExternBlock(Program& program);
    StructDecl* parseStruct();
    EnumDecl* parseEnum();
    GlobalVar* parseGlobal(bool isConst, bool inferred);
    void synchronizeDecl();

    // Types
    TypeExpr* parseType(bool inExpression = false);
    std::vector<TypeExpr*> parseTypeArgs();            // after '<', through the closing '>'
    std::vector<std::string> parseTypeParams();        // after '<': <T, U>
    void expectCloseAngle();                           // '>' (splits '>>' in List<List<int>>)
    bool looksLikeDecl() const;   // `Type name` at statement start?
    size_t skipType(size_t i) const;                   // token index after a type, or 0

    // Statements
    Stmt* parseStmt();
    BlockStmt* parseBlock();
    Stmt* parseVarDecl(bool isConst, bool inferred);
    Stmt* parseSimpleStmt();      // declaration or expression, without the `;`
    void synchronizeStmt();

    // Expressions
    Expr* parseAssignment();
    Expr* parseTernary();
    Expr* parseBinary(int minPrec);
    Expr* parseCast();
    Expr* parseUnary();
    Expr* parsePostfix(Expr* e);
    Expr* parsePrimary();

    std::vector<Token> tokens_;
    size_t pos_ = 0;
    AstArena& arena_;
    Diagnostics& diags_;
};

} // namespace ron
