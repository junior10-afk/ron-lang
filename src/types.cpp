#include "types.hpp"

namespace ron {

TypeContext::TypeContext() {
    void_ = make(TypeKind::Void, 0, "void");
    bool_ = make(TypeKind::Bool, 1, "bool");
    null_ = make(TypeKind::Null, 8, "null");
    i8_ = make(TypeKind::Int, 1, "i8", true);
    i16_ = make(TypeKind::Int, 2, "i16", true);
    i32_ = make(TypeKind::Int, 4, "i32", true);
    i64_ = make(TypeKind::Int, 8, "i64", true);
    u8_ = make(TypeKind::Int, 1, "u8");
    u16_ = make(TypeKind::Int, 2, "u16");
    u32_ = make(TypeKind::Int, 4, "u32");
    u64_ = make(TypeKind::Int, 8, "u64");
    f32_ = make(TypeKind::Float, 4, "f32");
    f64_ = make(TypeKind::Float, 8, "f64");

    builtins_ = {
        {"void", void_}, {"bool", bool_},
        {"i8", i8_},     {"i16", i16_},   {"i32", i32_}, {"i64", i64_},
        {"u8", u8_},     {"u16", u16_},   {"u32", u32_}, {"u64", u64_},
        {"f32", f32_},   {"f64", f64_},
        // Friendly aliases
        {"int", i32_},   {"uint", u32_},  {"float", f32_}, {"double", f64_},
        {"char", u8_},   {"byte", u8_},   {"usize", u64_}, {"isize", i64_},
    };
}

Type* TypeContext::make(TypeKind kind, uint32_t size, const std::string& name, bool isSigned) {
    auto t = std::make_unique<Type>();
    t->kind = kind;
    t->size = size;
    t->align = size == 0 ? 1 : size;
    t->isSigned = isSigned;
    t->name = name;
    types_.push_back(std::move(t));
    return types_.back().get();
}

Type* TypeContext::builtin(const std::string& name) const {
    auto it = builtins_.find(name);
    return it == builtins_.end() ? nullptr : it->second;
}

Type* TypeContext::pointerTo(Type* elem) {
    auto it = pointers_.find(elem);
    if (it != pointers_.end()) return it->second;
    Type* t = make(TypeKind::Pointer, 8, "");
    t->elem = elem;
    pointers_[elem] = t;
    return t;
}

Type* TypeContext::arrayOf(Type* elem, uint64_t count) {
    auto key = std::make_pair(elem, count);
    auto it = arrays_.find(key);
    if (it != arrays_.end()) return it->second;
    Type* t = make(TypeKind::Array, static_cast<uint32_t>(elem->size * count), "");
    t->elem = elem;
    t->count = count;
    t->align = elem->align;
    arrays_[key] = t;
    return t;
}

Type* TypeContext::sliceOf(Type* elem) {
    auto it = slices_.find(elem);
    if (it != slices_.end()) return it->second;
    Type* t = make(TypeKind::Slice, 16, "");
    t->elem = elem;
    t->align = 8;
    slices_[elem] = t;
    return t;
}

Type* TypeContext::functionType(const std::vector<Type*>& params, Type* ret) {
    auto key = std::make_pair(params, ret);
    auto it = functions_.find(key);
    if (it != functions_.end()) return it->second;
    Type* t = make(TypeKind::Function, 8, "");
    t->params = params;
    t->ret = ret;
    functions_[key] = t;
    return t;
}

Type* TypeContext::newStruct(const std::string& name, StructDecl* decl) {
    Type* t = make(TypeKind::Struct, 0, name);
    t->structDecl = decl;
    t->complete = false;
    return t;
}

Type* TypeContext::newEnum(const std::string& name, EnumDecl* decl) {
    Type* t = make(TypeKind::Int, 4, name, true);
    t->enumDecl = decl;
    return t;
}

std::string typeName(const Type* t) {
    if (!t) return "<unknown>";
    switch (t->kind) {
    case TypeKind::Pointer: return typeName(t->elem) + "*";
    case TypeKind::Array: return typeName(t->elem) + "[" + std::to_string(t->count) + "]";
    case TypeKind::Slice: return typeName(t->elem) + "[]";
    case TypeKind::Function: {
        std::string s = "fn(";
        for (size_t i = 0; i < t->params.size(); ++i) s += (i ? ", " : "") + typeName(t->params[i]);
        s += ")";
        if (t->ret && !t->ret->isVoid()) s += " -> " + typeName(t->ret);
        return s;
    }
    default: return t->name;
    }
}

} // namespace ron
