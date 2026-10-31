#include "parser.hpp"

namespace ron {

namespace {

// Binary operator precedence (higher binds tighter). 0 = not a binary operator.
int precedence(TokenKind k) {
    switch (k) {
    case TokenKind::PipePipe: return 1;
    case TokenKind::AmpAmp: return 2;
    case TokenKind::Pipe: return 3;
    case TokenKind::Caret: return 4;
    case TokenKind::Amp: return 5;
    case TokenKind::EqEq:
    case TokenKind::BangEq: return 6;
    case TokenKind::Less:
    case TokenKind::LessEq:
    case TokenKind::Greater:
    case TokenKind::GreaterEq: return 7;
    case TokenKind::Shl:
    case TokenKind::Shr: return 8;
    case TokenKind::Plus:
    case TokenKind::Minus: return 9;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent: return 10;
    default: return 0;
    }
}

BinaryOp toBinaryOp(TokenKind k) {
    switch (k) {
    case TokenKind::Plus: case TokenKind::PlusEq: return BinaryOp::Add;
    case TokenKind::Minus: case TokenKind::MinusEq: return BinaryOp::Sub;
    case TokenKind::Star: case TokenKind::StarEq: return BinaryOp::Mul;
    case TokenKind::Slash: case TokenKind::SlashEq: return BinaryOp::Div;
    case TokenKind::Percent: case TokenKind::PercentEq: return BinaryOp::Mod;
    case TokenKind::Amp: case TokenKind::AmpEq: return BinaryOp::BitAnd;
    case TokenKind::Pipe: case TokenKind::PipeEq: return BinaryOp::BitOr;
    case TokenKind::Caret: case TokenKind::CaretEq: return BinaryOp::BitXor;
    case TokenKind::Shl: case TokenKind::ShlEq: return BinaryOp::Shl;
    case TokenKind::Shr: case TokenKind::ShrEq: return BinaryOp::Shr;
    case TokenKind::EqEq: return BinaryOp::Eq;
    case TokenKind::BangEq: return BinaryOp::Ne;
    case TokenKind::Less: return BinaryOp::Lt;
    case TokenKind::LessEq: return BinaryOp::Le;
    case TokenKind::Greater: return BinaryOp::Gt;
    case TokenKind::GreaterEq: return BinaryOp::Ge;
    case TokenKind::AmpAmp: return BinaryOp::LogAnd;
    case TokenKind::PipePipe: return BinaryOp::LogOr;
    default: return BinaryOp::Add;
    }
}

bool isCompoundAssign(TokenKind k) {
    switch (k) {
    case TokenKind::PlusEq: case TokenKind::MinusEq: case TokenKind::StarEq:
    case TokenKind::SlashEq: case TokenKind::PercentEq: case TokenKind::AmpEq:
    case TokenKind::PipeEq: case TokenKind::CaretEq: case TokenKind::ShlEq:
    case TokenKind::ShrEq:
        return true;
    default:
        return false;
    }
}

// Tokens allowed after `fn` to declare an operator overload.
const char* overloadableOperator(TokenKind k) {
    switch (k) {
    case TokenKind::Plus: return "op+";
    case TokenKind::Minus: return "op-";
    case TokenKind::Star: return "op*";
    case TokenKind::Slash: return "op/";
    case TokenKind::Percent: return "op%";
    case TokenKind::EqEq: return "op==";
    case TokenKind::BangEq: return "op!=";
    case TokenKind::Less: return "op<";
    case TokenKind::LessEq: return "op<=";
    case TokenKind::Greater: return "op>";
    case TokenKind::GreaterEq: return "op>=";
    case TokenKind::Amp: return "op&";
    case TokenKind::Pipe: return "op|";
    case TokenKind::Caret: return "op^";
    default: return nullptr;
    }
}

} // namespace

Parser::Parser(std::vector<Token> tokens, AstArena& arena, Diagnostics& diags)
    : tokens_(std::move(tokens)), arena_(arena), diags_(diags) {
    // Error tokens were already reported by the lexer; drop them.
    std::erase_if(tokens_, [](const Token& t) { return t.is(TokenKind::Error); });
    if (tokens_.empty() || !tokens_.back().is(TokenKind::EndOfFile)) tokens_.push_back(Token{});
}

// ───────────────────────────── Token helpers ─────────────────────────────

const Token& Parser::peek(size_t ahead) const {
    size_t i = pos_ + ahead;
    return i < tokens_.size() ? tokens_[i] : tokens_.back();
}

const Token& Parser::advance() {
    const Token& t = tokens_[pos_];
    if (pos_ + 1 < tokens_.size()) ++pos_;
    return t;
}

bool Parser::match(TokenKind k) {
    if (!check(k)) return false;
    advance();
    return true;
}

const Token& Parser::expect(TokenKind k, const char* what) {
    if (check(k)) return advance();
    fail(peek(), std::string("expected ") + what);
}

void Parser::fail(const Token& at, const std::string& message) {
    std::string found = at.is(TokenKind::EndOfFile) ? "end of file"
                                                    : "'" + std::string(at.text) + "'";
    diags_.error(at.loc, message + ", found " + found);
    throw ParseError{};
}

// ───────────────────────────── Declarations ─────────────────────────────

void Parser::parseProgram(Program& program) {
    while (!check(TokenKind::EndOfFile)) {
        try {
            if (check(TokenKind::KwExtern) && peek(1).is(TokenKind::StringLiteral) &&
                peek(2).is(TokenKind::LBrace)) {
                parseExternBlock(program);
            } else {
                program.decls.push_back(parseDecl());
            }
        } catch (const ParseError&) {
            synchronizeDecl();
        }
    }
}

void Parser::synchronizeDecl() {
    // Skip to the next token that can start a declaration at brace depth 0.
    int depth = 0;
    while (!check(TokenKind::EndOfFile)) {
        TokenKind k = peek().kind;
        if (depth == 0 && (k == TokenKind::KwFn || k == TokenKind::KwStruct || k == TokenKind::KwEnum ||
                           k == TokenKind::KwImport || k == TokenKind::KwExtern || k == TokenKind::KwConst ||
                           k == TokenKind::KwVar || k == TokenKind::KwPrivate))
            return;
        advance();
        if (k == TokenKind::LBrace) ++depth;
        if (k == TokenKind::RBrace && depth > 0 && --depth == 0) return;
        if (k == TokenKind::Semicolon && depth == 0) return;
    }
}

Decl* Parser::parseDecl() {
    const Token& t = peek();
    switch (t.kind) {
    case TokenKind::KwImport: {
        advance();
        auto* imp = arena_.make<ImportDecl>(t.loc);
        imp->path = expect(TokenKind::StringLiteral, "a file path string after 'import'").strValue;
        expect(TokenKind::Semicolon, "';' after import");
        return imp;
    }
    case TokenKind::KwFn:
        if (peek(1).is(TokenKind::LParen)) return parseGlobal(false, false);   // fn(int) -> int callback = f;
        return parseFunc("");
    case TokenKind::KwExtern: {
        advance();
        std::string dll = expect(TokenKind::StringLiteral, "a DLL name after 'extern'").strValue;
        if (!check(TokenKind::KwFn)) fail(peek(), "expected 'fn' or '{' after extern \"...\"");
        return parseFunc(dll);
    }
    case TokenKind::KwStruct:
        return parseStruct();
    case TokenKind::KwEnum:
        return parseEnum();
    case TokenKind::KwConst:
        advance();
        return parseGlobal(true, check(TokenKind::Identifier) && peek(1).is(TokenKind::Eq));
    case TokenKind::KwVar:
        advance();
        return parseGlobal(false, true);
    case TokenKind::Identifier:
        return parseGlobal(false, false);
    default:
        fail(t, "expected a declaration (fn, struct, enum, const, import, extern or a global variable)");
    }
}

void Parser::parseExternBlock(Program& program) {
    advance();   // extern
    std::string dll = advance().strValue;
    expect(TokenKind::LBrace, "'{'");
    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
        if (!check(TokenKind::KwFn)) fail(peek(), "expected 'fn' inside extern block");
        program.decls.push_back(parseFunc(dll));
    }
    expect(TokenKind::RBrace, "'}' to close extern block");
}

FuncDecl* Parser::parseFunc(const std::string& externDll) {
    const Token& kw = expect(TokenKind::KwFn, "'fn'");
    auto* fn = arena_.make<FuncDecl>(kw.loc);
    fn->externDll = externDll;

    if (check(TokenKind::Identifier)) {
        fn->name = std::string(advance().text);
    } else if (const char* op = overloadableOperator(peek().kind)) {
        fn->name = op;
        advance();
    } else {
        fail(peek(), "expected a function name");
    }
    if (match(TokenKind::Less)) fn->typeParams = parseTypeParams();

    expect(TokenKind::LParen, "'(' after function name");
    if (!check(TokenKind::RParen)) {
        do {
            Param p;
            p.loc = peek().loc;
            p.typeExpr = parseType();
            p.name = std::string(expect(TokenKind::Identifier, "a parameter name").text);
            fn->params.push_back(std::move(p));
        } while (match(TokenKind::Comma));
    }
    expect(TokenKind::RParen, "')' after parameters");

    if (match(TokenKind::Arrow)) fn->returnTypeExpr = parseType();

    if (!externDll.empty()) {
        expect(TokenKind::Semicolon, "';' after extern function declaration");
    } else {
        fn->body = parseBlock();
    }
    return fn;
}

StructDecl* Parser::parseStruct() {
    const Token& kw = advance();
    auto* s = arena_.make<StructDecl>(kw.loc);
    s->name = std::string(expect(TokenKind::Identifier, "a struct name").text);
    if (match(TokenKind::Less)) s->typeParams = parseTypeParams();
    expect(TokenKind::LBrace, "'{' after struct name");
    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
        TypeExpr* type = parseType();
        // `float x, y, z;` declares several fields of the same type.
        do {
            FieldDecl f;
            f.loc = peek().loc;
            f.typeExpr = type;
            f.name = std::string(expect(TokenKind::Identifier, "a field name").text);
            s->fields.push_back(std::move(f));
        } while (match(TokenKind::Comma));
        expect(TokenKind::Semicolon, "';' after field");
    }
    expect(TokenKind::RBrace, "'}' to close struct");
    return s;
}

EnumDecl* Parser::parseEnum() {
    const Token& kw = advance();
    auto* e = arena_.make<EnumDecl>(kw.loc);
    e->name = std::string(expect(TokenKind::Identifier, "an enum name").text);
    expect(TokenKind::LBrace, "'{' after enum name");
    while (!check(TokenKind::RBrace)) {
        EnumMember m;
        m.loc = peek().loc;
        m.name = std::string(expect(TokenKind::Identifier, "an enum member name").text);
        if (match(TokenKind::Eq)) m.value = parseExpression();
        e->members.push_back(std::move(m));
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RBrace, "'}' to close enum");
    return e;
}

GlobalVar* Parser::parseGlobal(bool isConst, bool inferred) {
    auto* g = arena_.make<GlobalVar>(peek().loc);
    g->isConst = isConst;
    if (!inferred) g->typeExpr = parseType();
    g->name = std::string(expect(TokenKind::Identifier, "a variable name").text);
    if (match(TokenKind::Eq)) g->init = parseExpression();
    else if (isConst || inferred) fail(peek(), "expected '=' with a value");
    expect(TokenKind::Semicolon, "';' after global variable");
    return g;
}

// ───────────────────────────── Types ─────────────────────────────

TypeExpr* Parser::parseType(bool inExpression) {
    TypeExpr* t;
    if (check(TokenKind::KwFn)) {
        // Function type: fn(int, float) -> bool
        t = arena_.make<TypeExpr>(advance().loc);
        t->kind = TypeExpr::Kind::Function;
        expect(TokenKind::LParen, "'(' after 'fn' in a function type");
        if (!check(TokenKind::RParen)) {
            do {
                t->params.push_back(parseType());
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')' to close the parameter types");
        if (match(TokenKind::Arrow)) t->inner = parseType(inExpression);
        return t;
    }
    const Token& name = expect(TokenKind::Identifier, "a type name");
    t = arena_.make<TypeExpr>(name.loc);
    t->kind = TypeExpr::Kind::Named;
    t->name = std::string(name.text);
    // After `as`, `x as float * 2` is a multiplication, not a cast to float*.
    auto startsOperand = [&](const Token& tok) {
        switch (tok.kind) {
        case TokenKind::Identifier: case TokenKind::IntLiteral: case TokenKind::FloatLiteral:
        case TokenKind::CharLiteral: case TokenKind::StringLiteral: case TokenKind::LParen:
        case TokenKind::Minus: case TokenKind::Bang: case TokenKind::Tilde: case TokenKind::Amp:
        case TokenKind::KwTrue: case TokenKind::KwFalse: case TokenKind::KwNull: case TokenKind::KwSizeof:
            return true;
        default:
            return false;
        }
    };
    // Generic arguments (not after `as`, where `x as int < y` is a comparison).
    if (!inExpression && check(TokenKind::Less)) {
        advance();
        t->typeArgs = parseTypeArgs();
    }
    while (true) {
        if (check(TokenKind::Star) && !(inExpression && startsOperand(peek(1)))) {
            auto* p = arena_.make<TypeExpr>(advance().loc);
            p->kind = TypeExpr::Kind::Pointer;
            p->inner = t;
            t = p;
        } else if (check(TokenKind::LBracket) && peek(1).is(TokenKind::RBracket)) {
            auto* s = arena_.make<TypeExpr>(advance().loc);   // slice: T[]
            advance();
            s->kind = TypeExpr::Kind::Slice;
            s->inner = t;
            t = s;
        } else if (check(TokenKind::LBracket)) {
            auto* a = arena_.make<TypeExpr>(advance().loc);
            a->kind = TypeExpr::Kind::Array;
            a->inner = t;
            if (check(TokenKind::Identifier)) {
                a->sizeConst = std::string(advance().text);
            } else {
                const Token& n = expect(TokenKind::IntLiteral, "an array size");
                if (n.intValue == 0) fail(n, "array size must be at least 1");
                a->arraySize = n.intValue;
            }
            expect(TokenKind::RBracket, "']'");
            t = a;
        } else {
            return t;
        }
    }
}

std::vector<std::string> Parser::parseTypeParams() {
    std::vector<std::string> params;
    do {
        params.push_back(std::string(expect(TokenKind::Identifier, "a type parameter name").text));
    } while (match(TokenKind::Comma));
    expectCloseAngle();
    return params;
}

std::vector<TypeExpr*> Parser::parseTypeArgs() {
    std::vector<TypeExpr*> args;
    do {
        args.push_back(parseType());
    } while (match(TokenKind::Comma));
    expectCloseAngle();
    return args;
}

void Parser::expectCloseAngle() {
    if (match(TokenKind::Greater)) return;
    if (check(TokenKind::Shr)) {
        // `List<List<int>>`: consume one '>' of the '>>' token.
        Token& t = tokens_[pos_];
        t.kind = TokenKind::Greater;
        t.text = t.text.substr(1);
        ++t.loc.column;
        ++t.loc.offset;
        return;
    }
    fail(peek(), "expected '>' to close the type arguments");
}

// Skips `Name<Args>* [N]...` starting at token i; returns the index after it, or 0.
size_t Parser::skipType(size_t i) const {
    if (peek(i).is(TokenKind::KwFn) && peek(i + 1).is(TokenKind::LParen)) {
        // fn( ... ) [-> Type]
        int depth = 0;
        i += 1;
        do {
            TokenKind k = peek(i).kind;
            if (k == TokenKind::LParen) ++depth;
            else if (k == TokenKind::RParen) --depth;
            else if (k == TokenKind::EndOfFile || k == TokenKind::Semicolon || k == TokenKind::LBrace) return 0;
            ++i;
        } while (depth > 0);
        if (peek(i).is(TokenKind::Arrow)) return skipType(i + 1);
        return i;
    }
    if (!peek(i).is(TokenKind::Identifier)) return 0;
    ++i;
    if (peek(i).is(TokenKind::Less)) {
        int depth = 0;
        do {
            TokenKind k = peek(i).kind;
            if (k == TokenKind::Less) ++depth;
            else if (k == TokenKind::Greater) --depth;
            else if (k == TokenKind::Shr) depth -= 2;
            else if (k != TokenKind::Identifier && k != TokenKind::Comma && k != TokenKind::Star &&
                     k != TokenKind::LBracket && k != TokenKind::RBracket && k != TokenKind::IntLiteral)
                return 0;
            if (depth < 0) return 0;
            ++i;
        } while (depth > 0);
    }
    while (true) {
        if (peek(i).is(TokenKind::Star)) {
            ++i;
        } else if (peek(i).is(TokenKind::LBracket) && peek(i + 1).is(TokenKind::RBracket)) {
            i += 2;
        } else if (peek(i).is(TokenKind::LBracket) &&
                   (peek(i + 1).is(TokenKind::IntLiteral) || peek(i + 1).is(TokenKind::Identifier)) &&
                   peek(i + 2).is(TokenKind::RBracket)) {
            i += 3;
        } else {
            return i;
        }
    }
}

bool Parser::looksLikeDecl() const {
    // Type Identifier
    size_t i = skipType(0);
    return i != 0 && peek(i).is(TokenKind::Identifier);
}

// ───────────────────────────── Statements ─────────────────────────────

void Parser::synchronizeStmt() {
    int depth = 0;
    while (!check(TokenKind::EndOfFile)) {
        TokenKind k = peek().kind;
        if (k == TokenKind::RBrace) {
            if (depth == 0) return;   // let the enclosing block consume it
            --depth;
        }
        advance();
        if (k == TokenKind::LBrace) ++depth;
        if (k == TokenKind::Semicolon && depth == 0) return;
    }
}

BlockStmt* Parser::parseBlock() {
    const Token& open = expect(TokenKind::LBrace, "'{'");
    auto* block = arena_.make<BlockStmt>(open.loc);
    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
        try {
            block->stmts.push_back(parseStmt());
        } catch (const ParseError&) {
            synchronizeStmt();
        }
    }
    expect(TokenKind::RBrace, "'}' to close block");
    return block;
}

Stmt* Parser::parseVarDecl(bool isConst, bool inferred) {
    auto* v = arena_.make<VarDeclStmt>(peek().loc);
    v->isConst = isConst;
    // `var x = e;` and `const x = e;` infer the type from the initializer.
    if (!inferred) v->typeExpr = parseType();
    v->name = std::string(expect(TokenKind::Identifier, "a variable name").text);
    if (match(TokenKind::Eq)) v->init = parseExpression();
    else if (isConst || inferred) fail(peek(), inferred ? "expected '=' (the type is inferred from the value)"
                                                        : "expected '=' (a const needs a value)");
    return v;
}

Stmt* Parser::parseSimpleStmt() {
    if (check(TokenKind::KwVar)) {
        advance();
        return parseVarDecl(false, true);
    }
    if (check(TokenKind::KwConst)) {
        advance();
        bool inferred = check(TokenKind::Identifier) && peek(1).is(TokenKind::Eq);
        return parseVarDecl(true, inferred);
    }
    if (looksLikeDecl()) return parseVarDecl(false, false);
    auto* s = arena_.make<ExprStmt>(peek().loc);
    s->expr = parseExpression();
    return s;
}

Stmt* Parser::parseStmt() {
    // `name: while/for/do ...` labels a loop for `break name;` / `continue name;`.
    if (check(TokenKind::Identifier) && peek(1).is(TokenKind::Colon) &&
        (peek(2).is(TokenKind::KwWhile) || peek(2).is(TokenKind::KwFor) || peek(2).is(TokenKind::KwDo))) {
        std::string label(advance().text);
        advance();   // ':'
        Stmt* loop = parseStmt();
        if (loop->kind == StmtKind::While) loop->as<WhileStmt>()->label = label;
        else if (loop->kind == StmtKind::For) loop->as<ForStmt>()->label = label;
        else if (loop->kind == StmtKind::DoWhile) loop->as<DoWhileStmt>()->label = label;
        else if (loop->kind == StmtKind::ForIn) loop->as<ForInStmt>()->label = label;
        return loop;
    }
    const Token& t = peek();
    switch (t.kind) {
    case TokenKind::LBrace:
        return parseBlock();
    case TokenKind::KwDo: {
        advance();
        auto* s = arena_.make<DoWhileStmt>(t.loc);
        s->body = parseStmt();
        expect(TokenKind::KwWhile, "'while' after do-loop body");
        expect(TokenKind::LParen, "'(' after 'while'");
        s->cond = parseExpression();
        expect(TokenKind::RParen, "')' after condition");
        expect(TokenKind::Semicolon, "';' after do-while");
        return s;
    }
    case TokenKind::KwSwitch: {
        advance();
        auto* s = arena_.make<SwitchStmt>(t.loc);
        expect(TokenKind::LParen, "'(' after 'switch'");
        s->subject = parseExpression();
        expect(TokenKind::RParen, "')' after switch value");
        expect(TokenKind::LBrace, "'{' to open the switch");
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            SwitchCase sc;
            sc.loc = peek().loc;
            if (match(TokenKind::KwDefault)) {
                sc.isDefault = true;
            } else {
                expect(TokenKind::KwCase, "'case' or 'default'");
                do {
                    sc.values.push_back(parseExpression());
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::Colon, "':' after case value");
            while (!check(TokenKind::KwCase) && !check(TokenKind::KwDefault) && !check(TokenKind::RBrace) &&
                   !check(TokenKind::EndOfFile)) {
                try {
                    sc.body.push_back(parseStmt());
                } catch (const ParseError&) {
                    synchronizeStmt();
                }
            }
            s->cases.push_back(std::move(sc));
        }
        expect(TokenKind::RBrace, "'}' to close the switch");
        return s;
    }
    case TokenKind::KwDefer: {
        advance();
        auto* s = arena_.make<DeferStmt>(t.loc);
        s->body = parseStmt();
        return s;
    }
    case TokenKind::KwIf: {
        advance();
        auto* s = arena_.make<IfStmt>(t.loc);
        expect(TokenKind::LParen, "'(' after 'if'");
        s->cond = parseExpression();
        expect(TokenKind::RParen, "')' after condition");
        s->thenStmt = parseStmt();
        if (match(TokenKind::KwElse)) s->elseStmt = parseStmt();
        return s;
    }
    case TokenKind::KwWhile: {
        advance();
        auto* s = arena_.make<WhileStmt>(t.loc);
        expect(TokenKind::LParen, "'(' after 'while'");
        s->cond = parseExpression();
        expect(TokenKind::RParen, "')' after condition");
        s->body = parseStmt();
        return s;
    }
    case TokenKind::KwFor: {
        advance();
        expect(TokenKind::LParen, "'(' after 'for'");
        // for (var x in items) / for (Type x in a..b)
        bool forIn = check(TokenKind::KwVar) && peek(1).is(TokenKind::Identifier) && peek(2).is(TokenKind::KwIn);
        size_t typeEnd = forIn ? 0 : skipType(0);
        forIn = forIn || (typeEnd && peek(typeEnd).is(TokenKind::Identifier) && peek(typeEnd + 1).is(TokenKind::KwIn));
        if (forIn) {
            auto* fi = arena_.make<ForInStmt>(t.loc);
            if (!match(TokenKind::KwVar)) fi->varType = parseType();
            fi->name = std::string(expect(TokenKind::Identifier, "a loop variable name").text);
            expect(TokenKind::KwIn, "'in'");
            fi->iterable = parseExpression();
            if (match(TokenKind::DotDot)) fi->rangeEnd = parseExpression();
            expect(TokenKind::RParen, "')' after the for-in header");
            fi->body = parseStmt();
            return fi;
        }
        auto* s = arena_.make<ForStmt>(t.loc);
        if (!check(TokenKind::Semicolon)) s->init = parseSimpleStmt();
        expect(TokenKind::Semicolon, "';' after for-loop initializer");
        if (!check(TokenKind::Semicolon)) s->cond = parseExpression();
        expect(TokenKind::Semicolon, "';' after for-loop condition");
        if (!check(TokenKind::RParen)) s->step = parseExpression();
        expect(TokenKind::RParen, "')' after for-loop step");
        s->body = parseStmt();
        return s;
    }
    case TokenKind::KwReturn: {
        advance();
        auto* s = arena_.make<ReturnStmt>(t.loc);
        if (!check(TokenKind::Semicolon)) s->value = parseExpression();
        expect(TokenKind::Semicolon, "';' after return");
        return s;
    }
    case TokenKind::KwBreak: {
        advance();
        auto* s = arena_.make<BreakStmt>(t.loc);
        if (check(TokenKind::Identifier)) s->label = std::string(advance().text);
        expect(TokenKind::Semicolon, "';' after break");
        return s;
    }
    case TokenKind::KwContinue: {
        advance();
        auto* s = arena_.make<ContinueStmt>(t.loc);
        if (check(TokenKind::Identifier)) s->label = std::string(advance().text);
        expect(TokenKind::Semicolon, "';' after continue");
        return s;
    }
    case TokenKind::Semicolon: {
        advance();
        return arena_.make<BlockStmt>(t.loc);   // empty statement
    }
    default: {
        Stmt* s = parseSimpleStmt();
        expect(TokenKind::Semicolon, "';' after statement");
        return s;
    }
    }
}

// ───────────────────────────── Expressions ─────────────────────────────

Expr* Parser::parseExpression() { return parseAssignment(); }

Expr* Parser::parseAssignment() {
    Expr* lhs = parseTernary();
    TokenKind k = peek().kind;
    if (k == TokenKind::Eq || isCompoundAssign(k)) {
        const Token& op = advance();
        auto* a = arena_.make<AssignExpr>(op.loc);
        a->target = lhs;
        a->compound = k != TokenKind::Eq;
        a->op = toBinaryOp(k);
        a->value = parseAssignment();   // right associative
        return a;
    }
    return lhs;
}

Expr* Parser::parseTernary() {
    Expr* cond = parseBinary(1);
    if (!check(TokenKind::Question)) return cond;
    const Token& q = advance();
    auto* t = arena_.make<TernaryExpr>(q.loc);
    t->cond = cond;
    t->thenExpr = parseExpression();
    expect(TokenKind::Colon, "':' in conditional expression");
    t->elseExpr = parseTernary();
    return t;
}

Expr* Parser::parseBinary(int minPrec) {
    Expr* lhs = parseCast();
    while (true) {
        int prec = precedence(peek().kind);
        if (prec == 0 || prec < minPrec) return lhs;
        const Token& op = advance();
        Expr* rhs = parseBinary(prec + 1);   // left associative
        auto* b = arena_.make<BinaryExpr>(op.loc);
        b->op = toBinaryOp(op.kind);
        b->lhs = lhs;
        b->rhs = rhs;
        lhs = b;
    }
}

Expr* Parser::parseCast() {
    Expr* e = parseUnary();
    while (check(TokenKind::KwAs)) {
        const Token& kw = advance();
        auto* c = arena_.make<CastExpr>(kw.loc);
        c->operand = e;
        c->target = parseType(true);
        e = c;
    }
    return e;
}

Expr* Parser::parseUnary() {
    const Token& t = peek();
    UnaryOp op;
    switch (t.kind) {
    case TokenKind::Minus: op = UnaryOp::Neg; break;
    case TokenKind::Bang: op = UnaryOp::Not; break;
    case TokenKind::Tilde: op = UnaryOp::BitNot; break;
    case TokenKind::Amp: op = UnaryOp::AddrOf; break;
    case TokenKind::Star: op = UnaryOp::Deref; break;
    case TokenKind::PlusPlus: op = UnaryOp::PreInc; break;
    case TokenKind::MinusMinus: op = UnaryOp::PreDec; break;
    default:
        return parsePostfix(parsePrimary());
    }
    advance();
    auto* u = arena_.make<UnaryExpr>(t.loc);
    u->op = op;
    u->operand = parseUnary();
    return u;
}

Expr* Parser::parsePostfix(Expr* e) {
    while (true) {
        const Token& t = peek();
        if (t.is(TokenKind::LParen)) {
            advance();
            auto* c = arena_.make<CallExpr>(t.loc);
            c->callee = e;
            if (!check(TokenKind::RParen)) {
                do {
                    c->args.push_back(parseExpression());
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RParen, "')' after arguments");
            e = c;
        } else if (t.is(TokenKind::LBracket)) {
            advance();
            Expr* first = check(TokenKind::DotDot) ? nullptr : parseExpression();
            if (match(TokenKind::DotDot)) {
                // Slicing: base[lo..hi], with either bound optional
                auto* sl = arena_.make<SliceExpr>(t.loc);
                sl->base = e;
                sl->lo = first;
                if (!check(TokenKind::RBracket)) sl->hi = parseExpression();
                expect(TokenKind::RBracket, "']'");
                e = sl;
                continue;
            }
            auto* ix = arena_.make<IndexExpr>(t.loc);
            ix->base = e;
            ix->index = first;
            expect(TokenKind::RBracket, "']'");
            e = ix;
        } else if (t.is(TokenKind::Dot)) {
            advance();
            auto* m = arena_.make<MemberExpr>(t.loc);
            m->base = e;
            m->name = std::string(expect(TokenKind::Identifier, "a field or method name after '.'").text);
            e = m;
        } else if (t.is(TokenKind::PlusPlus) || t.is(TokenKind::MinusMinus)) {
            advance();
            auto* u = arena_.make<UnaryExpr>(t.loc);
            u->op = t.is(TokenKind::PlusPlus) ? UnaryOp::PostInc : UnaryOp::PostDec;
            u->operand = e;
            e = u;
        } else {
            return e;
        }
    }
}

Expr* Parser::parsePrimary() {
    const Token& t = peek();
    switch (t.kind) {
    case TokenKind::IntLiteral:
    case TokenKind::CharLiteral: {
        advance();
        auto* e = arena_.make<IntLitExpr>(t.loc);
        e->value = t.intValue;
        return e;
    }
    case TokenKind::FloatLiteral: {
        advance();
        auto* e = arena_.make<FloatLitExpr>(t.loc);
        e->value = t.floatValue;
        e->untyped = !t.isF32;
        return e;
    }
    case TokenKind::StringLiteral: {
        advance();
        auto* e = arena_.make<StringLitExpr>(t.loc);
        e->value = t.strValue;
        // Adjacent string literals are concatenated: "abc" "def"
        while (check(TokenKind::StringLiteral)) e->value += advance().strValue;
        return e;
    }
    case TokenKind::KwTrue:
    case TokenKind::KwFalse: {
        advance();
        auto* e = arena_.make<BoolLitExpr>(t.loc);
        e->value = t.is(TokenKind::KwTrue);
        return e;
    }
    case TokenKind::KwNull:
        advance();
        return arena_.make<NullLitExpr>(t.loc);
    case TokenKind::KwSizeof: {
        advance();
        auto* e = arena_.make<SizeofExpr>(t.loc);
        expect(TokenKind::LParen, "'(' after sizeof");
        e->target = parseType();
        expect(TokenKind::RParen, "')' after sizeof type");
        return e;
    }
    case TokenKind::LParen: {
        advance();
        Expr* e = parseExpression();
        expect(TokenKind::RParen, "')'");
        return e;
    }
    case TokenKind::LBracket: {
        advance();
        auto* a = arena_.make<ArrayLitExpr>(t.loc);
        while (!check(TokenKind::RBracket)) {
            a->elems.push_back(parseExpression());
            if (!match(TokenKind::Comma)) break;
        }
        expect(TokenKind::RBracket, "']' to close the array literal");
        return a;
    }
    case TokenKind::Identifier: {
        advance();
        std::vector<TypeExpr*> typeArgs;
        if (check(TokenKind::ColonColon) && peek(1).is(TokenKind::Less)) {
            // Explicit generic arguments: make::<int>(), Pair::<int, float>{1, 2.0}
            advance();
            advance();
            typeArgs = parseTypeArgs();
        }
        if (typeArgs.empty() && check(TokenKind::ColonColon)) {
            advance();
            auto* p = arena_.make<PathExpr>(t.loc);
            p->scope = std::string(t.text);
            p->member = std::string(expect(TokenKind::Identifier, "a name after '::'").text);
            return p;
        }
        if (check(TokenKind::LBrace)) {
            advance();
            auto* s = arena_.make<StructLitExpr>(t.loc);
            s->typeName = std::string(t.text);
            s->typeArgs = typeArgs;
            if (!check(TokenKind::RBrace)) {
                do {
                    if (check(TokenKind::RBrace)) break;   // trailing comma
                    std::string fieldName;
                    if (check(TokenKind::Identifier) && peek(1).is(TokenKind::Colon)) {   // `x: 1.0`
                        fieldName = std::string(advance().text);
                        advance();
                    }
                    s->fieldNames.push_back(fieldName);
                    s->fields.push_back(parseExpression());
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RBrace, "'}' to close struct literal");
            return s;
        }
        auto* id = arena_.make<IdentExpr>(t.loc);
        id->name = std::string(t.text);
        id->typeArgs = typeArgs;
        return id;
    }
    default:
        fail(t, "expected an expression");
    }
}

} // namespace ron
