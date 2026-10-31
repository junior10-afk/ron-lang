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
    Type* inferType(Expr*& e);
    int findBreakTarget(const std::string& label, bool isContinue, SourceLoc loc);
    void checkLoopBody(Stmt* body, const std::string& label);
    Expr* checkArrayLit(ArrayLitExpr* e);
    Expr* checkSlice(SliceExpr* e);
    Expr* finishMember(MemberExpr* e);
    Expr* indirectCall(CallExpr* e, Expr* callee);
    Expr* functionValue(IdentExpr* e, std::vector<FuncDecl*>& overloads);
    void lowerForIn(ForInStmt* f);
    LocalVar* hiddenLocal(const char* name, Type* t, SourceLoc loc);
    Expr* mkIdent(LocalVar* v, SourceLoc loc);
    Expr* mkInt(uint64_t value, Type* t, SourceLoc loc);
    Stmt* mkVarDecl(LocalVar* v, Expr* init, SourceLoc loc);

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
                              bool reportErrors, Expr* ufcsBase = nullptr,
                              const std::vector<Type*>* explicitTypeArgs = nullptr);

    // Generics (monomorphization)
    Type* instantiateStruct(StructDecl* g, const std::vector<Type*>& args);
    FuncDecl* instantiateFunction(FuncDecl* g, const std::vector<Type*>& args, SourceLoc at);
    bool deduceTypeArgs(FuncDecl* f, const std::vector<Expr*>& args, const std::vector<Type*>* explicitArgs,
                        std::vector<Type*>& out);
    bool unify(TypeExpr* p, Type* a, const std::vector<std::string>& params, std::map<std::string, Type*>& env);
    void pushTypeEnv(const std::vector<std::string>& params, const std::vector<Type*>& args);
    void popTypeEnv() { typeEnv_.pop_back(); }
    std::string instanceName(const std::string& base, const std::vector<Type*>& args);

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

    std::vector<std::map<std::string, Type*>> typeEnv_;   // innermost frame: active type parameters
    std::map<std::pair<StructDecl*, std::vector<Type*>>, StructDecl*> structInstances_;
    std::map<std::pair<FuncDecl*, std::vector<Type*>>, FuncDecl*> funcInstances_;
    std::vector<FuncDecl*> pendingInstances_;   // instantiated, body not yet checked
    std::map<FuncDecl*, SourceLoc> instantiatedAt_;
    std::vector<FuncDecl*> globalRefs_;   // functions referenced outside any function (DCE roots)

    std::vector<std::map<std::string, LocalVar*>> scopes_;
    FuncDecl* currentFunc_ = nullptr;
    int loopDepth_ = 0;     // loop nesting (register allocation weights)
    int deferDepth_ = 0;
    struct BreakTarget {
        std::string label;
        enum Kind { Loop, Switch, Barrier } kind;   // Barrier: a defer body (cannot be jumped out of)
    };
    std::vector<BreakTarget> breakTargets_;
};

} // namespace ron
