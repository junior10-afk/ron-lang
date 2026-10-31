#pragma once

// Abstract syntax tree. Nodes are owned by an AstArena and referenced by raw
// pointers. Sema annotates expressions in place (type, resolved symbols) and may
// replace children (e.g. to insert implicit casts), so child pointers are mutable.

#include "token.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ron {

struct Type;
struct FuncDecl;
struct StructDecl;
struct GlobalVar;
struct LocalVar;

// ───────────────────────────── Arena ─────────────────────────────

struct AstNode {
    SourceLoc loc;
    virtual ~AstNode() = default;
};

class AstArena {
public:
    template <typename T, typename... Args>
    T* make(SourceLoc loc, Args&&... args) {
        auto node = std::make_unique<T>(std::forward<Args>(args)...);
        node->loc = loc;
        T* raw = node.get();
        nodes_.push_back(std::move(node));
        return raw;
    }

private:
    std::vector<std::unique_ptr<AstNode>> nodes_;
};

// ───────────────────────────── Type syntax ─────────────────────────────

struct TypeExpr : AstNode {
    enum class Kind { Named, Pointer, Array, Slice, Function } kind = Kind::Named;
    std::string name;            // Named
    TypeExpr* inner = nullptr;   // Pointer, Array, Slice; Function: return type (null = void)
    std::vector<TypeExpr*> params;     // Function parameter types
    uint64_t arraySize = 0;      // Array: literal size...
    std::string sizeConst;       // ...or the name of an integer const: float[MAX] a;
    std::vector<TypeExpr*> typeArgs;   // Named generic instance: List<int>
};

// ───────────────────────────── Expressions ─────────────────────────────

enum class ExprKind {
    IntLit, FloatLit, StringLit, BoolLit, NullLit,
    Ident, Path, Unary, Binary, Assign, Ternary,
    Call, Index, Member, Cast, Sizeof, StructLit, ArrayLit, Slice,
};

enum class UnaryOp { Neg, Not, BitNot, AddrOf, Deref, PreInc, PreDec, PostInc, PostDec };

enum class BinaryOp {
    Add, Sub, Mul, Div, Mod,
    BitAnd, BitOr, BitXor, Shl, Shr,
    Eq, Ne, Lt, Le, Gt, Ge,
    LogAnd, LogOr,
};

const char* binaryOpText(BinaryOp op);
const char* unaryOpText(UnaryOp op);

struct Expr : AstNode {
    ExprKind kind;
    Type* type = nullptr;        // set by sema
    bool isLvalue = false;       // set by sema
    explicit Expr(ExprKind k) : kind(k) {}

    template <typename T> T* as() { return static_cast<T*>(this); }
    template <typename T> const T* as() const { return static_cast<const T*>(this); }
};

struct IntLitExpr : Expr {
    uint64_t value = 0;
    bool negative = false;       // set when sema folds `-literal`
    bool untyped = true;         // literal without a fixed type yet
    IntLitExpr() : Expr(ExprKind::IntLit) {}
};

struct FloatLitExpr : Expr {
    double value = 0;
    bool untyped = true;         // false for `1.5f`
    FloatLitExpr() : Expr(ExprKind::FloatLit) {}
};

struct StringLitExpr : Expr {
    std::string value;
    StringLitExpr() : Expr(ExprKind::StringLit) {}
};

struct BoolLitExpr : Expr {
    bool value = false;
    BoolLitExpr() : Expr(ExprKind::BoolLit) {}
};

struct NullLitExpr : Expr {
    NullLitExpr() : Expr(ExprKind::NullLit) {}
};

struct IdentExpr : Expr {
    std::string name;
    std::vector<TypeExpr*> typeArgs;   // explicit generic arguments: make::<int>()
    LocalVar* local = nullptr;   // resolved by sema (one of these)
    GlobalVar* global = nullptr;
    FuncDecl* func = nullptr;    // a function used as a value (its address)
    IdentExpr() : Expr(ExprKind::Ident) {}
};

// `Enum::Member`
struct PathExpr : Expr {
    std::string scope;
    std::string member;
    PathExpr() : Expr(ExprKind::Path) {}
};

struct UnaryExpr : Expr {
    UnaryOp op;
    Expr* operand = nullptr;
    FuncDecl* overload = nullptr;   // operator overload chosen by sema
    UnaryExpr() : Expr(ExprKind::Unary) {}
};

struct BinaryExpr : Expr {
    BinaryOp op;
    Expr* lhs = nullptr;
    Expr* rhs = nullptr;
    FuncDecl* overload = nullptr;
    BinaryExpr() : Expr(ExprKind::Binary) {}
};

// `a = b`, `a += b` (op is set for compound assignment)
struct AssignExpr : Expr {
    bool compound = false;
    BinaryOp op = BinaryOp::Add;
    Expr* target = nullptr;
    Expr* value = nullptr;          // for overloaded compound ops sema rewrites this to `target op value`
    AssignExpr() : Expr(ExprKind::Assign) {}
};

struct TernaryExpr : Expr {
    Expr* cond = nullptr;
    Expr* thenExpr = nullptr;
    Expr* elseExpr = nullptr;
    TernaryExpr() : Expr(ExprKind::Ternary) {}
};

enum class Intrinsic { None, SqrtF32, SqrtF64 };

struct CallExpr : Expr {
    Expr* callee = nullptr;
    std::vector<Expr*> args;
    FuncDecl* func = nullptr;       // resolved target (direct call)
    Expr* calleeValue = nullptr;    // indirect call through a function value
    Intrinsic intrinsic = Intrinsic::None;
    CallExpr() : Expr(ExprKind::Call) {}
};

// `base[lo..hi]`, `base[..]`, `base[lo..]`, `base[..hi]` → a slice
struct SliceExpr : Expr {
    Expr* base = nullptr;
    Expr* lo = nullptr;             // null = 0
    Expr* hi = nullptr;             // null = length
    SliceExpr() : Expr(ExprKind::Slice) {}
};

struct IndexExpr : Expr {
    Expr* base = nullptr;
    Expr* index = nullptr;
    IndexExpr() : Expr(ExprKind::Index) {}
};

struct MemberExpr : Expr {
    Expr* base = nullptr;
    std::string name;
    bool throughPointer = false;    // `p.x` with p a pointer: auto-deref
    uint32_t offset = 0;            // field byte offset
    MemberExpr() : Expr(ExprKind::Member) {}
};

struct CastExpr : Expr {
    Expr* operand = nullptr;
    TypeExpr* target = nullptr;     // null for implicit casts inserted by sema
    CastExpr() : Expr(ExprKind::Cast) {}
};

struct SizeofExpr : Expr {
    TypeExpr* target = nullptr;
    uint64_t size = 0;
    SizeofExpr() : Expr(ExprKind::Sizeof) {}
};

// `Vec3{1.0, 2.0, 3.0}`, `Vec3{x: 1.0, z: 2.0}` or `Vec3{}` (zero-initialized).
// After sema, `fields` has one entry per struct field (null = zero).
struct StructLitExpr : Expr {
    std::string typeName;
    std::vector<TypeExpr*> typeArgs;   // Pair::<int, float>{1, 2.0}
    std::vector<Expr*> fields;
    std::vector<std::string> fieldNames;   // parallel to `fields` for named initializers
    StructLitExpr() : Expr(ExprKind::StructLit) {}
};

// `[1, 2, 3]`
struct ArrayLitExpr : Expr {
    std::vector<Expr*> elems;
    ArrayLitExpr() : Expr(ExprKind::ArrayLit) {}
};

// ───────────────────────────── Statements ─────────────────────────────

enum class StmtKind { Block, VarDecl, Expr, If, While, For, Return, Break, Continue, Switch, Defer, DoWhile, ForIn };

struct Stmt : AstNode {
    StmtKind kind;
    explicit Stmt(StmtKind k) : kind(k) {}
    template <typename T> T* as() { return static_cast<T*>(this); }
};

struct BlockStmt : Stmt {
    std::vector<Stmt*> stmts;
    BlockStmt() : Stmt(StmtKind::Block) {}
};

struct VarDeclStmt : Stmt {
    TypeExpr* typeExpr = nullptr;   // null for `var x = ...` (inferred)
    std::string name;
    Expr* init = nullptr;
    bool isConst = false;
    LocalVar* var = nullptr;        // set by sema
    VarDeclStmt() : Stmt(StmtKind::VarDecl) {}
};

struct ExprStmt : Stmt {
    Expr* expr = nullptr;
    ExprStmt() : Stmt(StmtKind::Expr) {}
};

struct IfStmt : Stmt {
    Expr* cond = nullptr;
    Stmt* thenStmt = nullptr;
    Stmt* elseStmt = nullptr;
    IfStmt() : Stmt(StmtKind::If) {}
};

struct WhileStmt : Stmt {
    Expr* cond = nullptr;
    Stmt* body = nullptr;
    std::string label;              // `outer: while (...)`
    WhileStmt() : Stmt(StmtKind::While) {}
};

struct DoWhileStmt : Stmt {
    Stmt* body = nullptr;
    Expr* cond = nullptr;
    std::string label;
    DoWhileStmt() : Stmt(StmtKind::DoWhile) {}
};

struct ForStmt : Stmt {
    Stmt* init = nullptr;           // VarDecl or Expr statement, may be null
    Expr* cond = nullptr;           // may be null (infinite loop)
    Expr* step = nullptr;           // may be null
    Stmt* body = nullptr;
    std::string label;
    ForStmt() : Stmt(StmtKind::For) {}
};

struct ReturnStmt : Stmt {
    Expr* value = nullptr;
    ReturnStmt() : Stmt(StmtKind::Return) {}
};

struct BreakStmt : Stmt {
    std::string label;              // empty = innermost loop or switch
    BreakStmt() : Stmt(StmtKind::Break) {}
};

struct ContinueStmt : Stmt {
    std::string label;
    ContinueStmt() : Stmt(StmtKind::Continue) {}
};

struct SwitchCase {
    std::vector<Expr*> values;      // empty for `default`
    std::vector<Stmt*> body;
    SourceLoc loc;
    bool isDefault = false;
};

// No fallthrough: each case ends at the next `case`. `break` leaves the switch.
struct SwitchStmt : Stmt {
    Expr* subject = nullptr;
    std::vector<SwitchCase> cases;
    SwitchStmt() : Stmt(StmtKind::Switch) {}
};

// `for (var x in items)`, `for (var p in &items)`, `for (int i in 0..n)`.
// Sema lowers it to an ordinary indexed loop stored in `lowered`.
struct ForInStmt : Stmt {
    TypeExpr* varType = nullptr;    // null = inferred
    std::string name;
    Expr* iterable = nullptr;       // range start when rangeEnd is set
    Expr* rangeEnd = nullptr;
    Stmt* body = nullptr;
    std::string label;
    Stmt* lowered = nullptr;
    ForInStmt() : Stmt(StmtKind::ForIn) {}
};

// Runs `body` when the enclosing block exits (normally, or via return/break/continue).
struct DeferStmt : Stmt {
    Stmt* body = nullptr;
    DeferStmt() : Stmt(StmtKind::Defer) {}
};

// ───────────────────────────── Declarations ─────────────────────────────

// A local variable or parameter (created by sema, storage assigned by codegen).
struct LocalVar {
    std::string name;
    Type* type = nullptr;
    bool isConst = false;
    bool isParam = false;
    bool indirect = false;          // slot holds a pointer to the value (large struct params)
    bool addressTaken = false;      // &v was used: must live in memory
    double useWeight = 0;           // uses weighted by loop depth (register allocation priority)
    int32_t frameOffset = 0;        // rbp-relative, set by codegen
    int8_t reg = -1;                // allocated register (GPR or XMM number), -1 = stack slot
};

struct Param {
    TypeExpr* typeExpr = nullptr;
    std::string name;
    SourceLoc loc;
};

struct Decl : AstNode {
    enum class Kind { Func, Struct, Enum, Global, Import } declKind;
    explicit Decl(Kind k) : declKind(k) {}
};

struct FuncDecl : Decl {
    std::string name;               // operator overloads use "op+", "op-", ...
    std::vector<std::string> typeParams;  // generic template: fn push<T>(...)
    FuncDecl* genericOrigin = nullptr;    // set on instances
    std::vector<Type*> typeArgs;          // instance arguments
    std::vector<Param> params;
    TypeExpr* returnTypeExpr = nullptr;   // null = void
    BlockStmt* body = nullptr;            // null for extern functions
    std::string externDll;                // non-empty for imported functions

    // Set by sema
    std::vector<Type*> paramTypes;
    Type* returnType = nullptr;
    std::vector<LocalVar*> paramVars;
    std::vector<LocalVar*> locals;        // every local declared in the body
    std::vector<FuncDecl*> callees;       // direct calls (for dead-code elimination)
    bool used = false;                    // reachable from main

    FuncDecl() : Decl(Kind::Func) {}
};

struct FieldDecl {
    TypeExpr* typeExpr = nullptr;
    std::string name;
    SourceLoc loc;
    Type* type = nullptr;
    uint32_t offset = 0;
};

struct StructDecl : Decl {
    std::string name;
    std::vector<FieldDecl> fields;
    Type* type = nullptr;
    std::vector<std::string> typeParams;  // generic template: struct List<T>
    StructDecl* genericOrigin = nullptr;  // set on instances
    std::vector<Type*> typeArgs;          // instance arguments
    StructDecl() : Decl(Kind::Struct) {}
};

struct EnumMember {
    std::string name;
    Expr* value = nullptr;          // optional explicit value
    SourceLoc loc;
    int64_t resolved = 0;
};

struct EnumDecl : Decl {
    std::string name;
    std::vector<EnumMember> members;
    Type* type = nullptr;
    EnumDecl() : Decl(Kind::Enum) {}
};

// Top-level variable or `const`.
struct GlobalVar : Decl {
    TypeExpr* typeExpr = nullptr;
    std::string name;
    Expr* init = nullptr;
    bool isConst = false;
    Type* type = nullptr;
    // Constant value computed by sema (consts and global initializers)
    bool hasValue = false;
    bool isFloatValue = false;
    uint64_t intValue = 0;
    double floatValue = 0;
    uint32_t dataOffset = 0;        // set by codegen
    GlobalVar() : Decl(Kind::Global) {}
};

struct ImportDecl : Decl {
    std::string path;
    ImportDecl() : Decl(Kind::Import) {}
};

struct Program {
    std::vector<Decl*> decls;
};

// Deep copies of unchecked AST (used to instantiate generic functions).
Expr* cloneExpr(AstArena& arena, Expr* e);
Stmt* cloneStmt(AstArena& arena, Stmt* s);

} // namespace ron
