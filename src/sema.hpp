#pragma once

#include "ast.hpp"
#include "diagnostics.hpp"
#include "types.hpp"

#include <map>
#include <string>
#include <vector>

namespace ron {

// Semantic analysis: resolves names and types, checks every expression,
// picks overloads, inserts implicit conversions and folds constants.
// After a successful run the AST is fully typed and ready for codegen.
class Sema {
public:
    Sema(Program& program, AstArena& arena, TypeContext& types, Diagnostics& diags);

    bool run();

    // Results
    std::vector<FuncDecl*> functions;   // every function with a body
    std::vector<FuncDecl*> externs;     // imported functions that are called
    std::vector<GlobalVar*> globals;    // mutable globals (consts are folded away)
    FuncDecl* mainFunc = nullptr;

private:
    // Declarations
    void collect();
    Type* resolveType(TypeExpr* t);
    void layoutStruct(StructDecl* s);
    void resolveEnum(EnumDecl* e);
    void resolveGlobal(GlobalVar* g);
    void resolveSignature(FuncDecl* f);
    void checkFunction(FuncDecl* f);

    // Statements
    void checkStmt(Stmt* s);
    void checkBlock(BlockStmt* b, bool newScope = true);
    Expr* checkCondition(Expr* e);
    bool alwaysReturns(Stmt* s);

    // Expressions
    Expr* check(Expr* e);
    Expr* checkIdent(IdentExpr* e);
    Expr* checkPath(PathExpr* e);
    Expr* checkUnary(UnaryExpr* e);
    Expr* checkBinary(BinaryExpr* e);
    Expr* checkAssign(AssignExpr* e);
    Expr* checkTernary(TernaryExpr* e);
    Expr* checkCall(CallExpr* e);
    Expr* checkIndex(IndexExpr* e);
    Expr* checkMember(MemberExpr* e);
    Expr* checkCast(CastExpr* e);
    Expr* checkStructLit(StructLitExpr* e);

    // Conversions
    int conversionCost(Expr* e, Type* to);
    Expr* coerce(Expr* e, Type* to, const char* context);
    Expr* implicitCast(Expr* e, Type* to);
    Expr* foldCast(Expr* e, Type* to, bool explicitCast);
    bool castAllowed(Type* from, Type* to);
    Expr* toOffset(Expr* e);
    Expr* fold(BinaryExpr* e);
    void setUntypedIntType(IntLitExpr* e);

    // Overloads
    FuncDecl* resolveOverload(const std::string& name, std::vector<Expr*>& args, SourceLoc loc,
                              bool reportErrors, Expr* ufcsBase = nullptr);

    // Scopes
    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }
    LocalVar* lookupLocal(const std::string& name);
    LocalVar* declareLocal(const std::string& name, Type* type, SourceLoc loc, bool isConst);

    Expr* errorExpr(Expr* e) { e->type = nullptr; return e; }

    Program& program_;
    AstArena& arena_;
    TypeContext& types_;
    Diagnostics& diags_;

    std::map<std::string, StructDecl*> structs_;
    std::map<std::string, EnumDecl*> enums_;
    std::map<std::string, GlobalVar*> globalsByName_;
    std::map<std::string, std::vector<FuncDecl*>> funcs_;
    std::map<GlobalVar*, int> globalState_;   // 0 = pending, 1 = resolving, 2 = done
    std::map<StructDecl*, int> structState_;
    std::vector<std::unique_ptr<LocalVar>> localStorage_;

    std::vector<std::map<std::string, LocalVar*>> scopes_;
    FuncDecl* currentFunc_ = nullptr;
    int loopDepth_ = 0;
};

} // namespace ron
