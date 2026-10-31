#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ron {

struct StructDecl;
struct EnumDecl;

enum class TypeKind { Void, Bool, Int, Float, Pointer, Array, Struct, Null };

struct Type {
    TypeKind kind = TypeKind::Void;
    uint32_t size = 0;
    uint32_t align = 1;
    bool isSigned = false;        // Int
    Type* elem = nullptr;         // Pointer, Array
    uint64_t count = 0;           // Array
    StructDecl* structDecl = nullptr;
    EnumDecl* enumDecl = nullptr; // Int types that come from an enum
    bool complete = true;         // false while a struct layout is being computed
    std::string name;

    bool isVoid() const { return kind == TypeKind::Void; }
    bool isBool() const { return kind == TypeKind::Bool; }
    bool isInt() const { return kind == TypeKind::Int; }
    bool isFloat() const { return kind == TypeKind::Float; }
    bool isNumeric() const { return isInt() || isFloat(); }
    bool isPointer() const { return kind == TypeKind::Pointer; }
    bool isArray() const { return kind == TypeKind::Array; }
    bool isStruct() const { return kind == TypeKind::Struct; }
    bool isNull() const { return kind == TypeKind::Null; }
    // Values that live in memory and are handled by address in codegen.
    bool isAggregate() const { return isStruct() || isArray(); }
    // Fits in a general-purpose register.
    bool isScalar() const { return isBool() || isInt() || isPointer() || isNull(); }
    bool isVoidPointer() const { return isPointer() && elem->isVoid(); }
};

// Owns all types and interns derived ones (pointers, arrays).
class TypeContext {
public:
    TypeContext();

    Type* voidType() { return void_; }
    Type* boolType() { return bool_; }
    Type* nullType() { return null_; }
    Type* i8() { return i8_; }
    Type* i16() { return i16_; }
    Type* i32() { return i32_; }
    Type* i64() { return i64_; }
    Type* u8() { return u8_; }
    Type* u16() { return u16_; }
    Type* u32() { return u32_; }
    Type* u64() { return u64_; }
    Type* f32() { return f32_; }
    Type* f64() { return f64_; }

    Type* pointerTo(Type* elem);
    Type* arrayOf(Type* elem, uint64_t count);
    Type* newStruct(const std::string& name, StructDecl* decl);
    Type* newEnum(const std::string& name, EnumDecl* decl);

    // Built-in type names: int, float, u8, ...; returns null when unknown.
    Type* builtin(const std::string& name) const;

private:
    Type* make(TypeKind kind, uint32_t size, const std::string& name, bool isSigned = false);

    std::vector<std::unique_ptr<Type>> types_;
    std::map<std::string, Type*> builtins_;
    std::map<Type*, Type*> pointers_;
    std::map<std::pair<Type*, uint64_t>, Type*> arrays_;
    Type *void_, *bool_, *null_, *i8_, *i16_, *i32_, *i64_, *u8_, *u16_, *u32_, *u64_, *f32_, *f64_;
};

std::string typeName(const Type* t);

} // namespace ron
