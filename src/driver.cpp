#include "driver.hpp"

#include "codegen.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"

#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace ron {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

namespace {

std::string canonicalPath(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return (ec ? p : c).string();
}

// `import "x.ron"` is resolved relative to the importing file, then to the std root.
std::string resolveImport(const std::string& path, const fs::path& fromDir, const std::string& stdRoot) {
    fs::path local = fromDir / path;
    if (fs::exists(local)) return canonicalPath(local);
    if (!stdRoot.empty()) {
        fs::path fromStd = fs::path(stdRoot) / path;
        if (fs::exists(fromStd)) return canonicalPath(fromStd);
    }
    return {};
}

} // namespace

CompileResult compile(const CompileOptions& options, SourceManager& sources, Diagnostics& diags) {
    CompileResult result;
    AstArena arena;
    Program program;

    std::deque<std::pair<std::string, SourceLoc>> queue;
    std::set<std::string> seen;
    std::map<std::string, uint32_t> fileIds;
    std::vector<std::pair<ImportDecl*, std::string>> pendingImports;
    std::string root = canonicalPath(options.input);
    queue.push_back({root, SourceLoc{0, 0, 0, UINT32_MAX}});
    seen.insert(root);
    // The prelude (Any, TypeInfo) is part of every program.
    std::string prelude = resolveImport("std/prelude.ron", fs::path(root).parent_path(), options.stdRoot);
    if (!prelude.empty() && seen.insert(prelude).second) queue.push_back({prelude, SourceLoc{0, 0, 0, UINT32_MAX}});

    while (!queue.empty()) {
        auto [path, from] = queue.front();
        queue.pop_front();
        std::string text;
        if (!readFile(path, text)) {
            diags.error(from, "cannot open '" + path + "'");
            continue;
        }
        result.sourceFiles.push_back(path);
        uint32_t id = sources.add(path, std::move(text));
        Lexer lexer(sources.get(id), id, diags);
        Parser parser(lexer.tokenizeAll(), arena, diags);
        size_t first = program.decls.size();
        parser.parseProgram(program);

        fs::path dir = fs::path(path).parent_path();
        for (size_t i = first; i < program.decls.size(); ++i) {
            if (program.decls[i]->declKind != Decl::Kind::Import) continue;
            auto* imp = static_cast<ImportDecl*>(program.decls[i]);
            std::string resolved = resolveImport(imp->path, dir, options.stdRoot);
            if (resolved.empty()) {
                diags.error(imp->loc, "cannot find imported file '" + imp->path + "'");
                continue;
            }
            pendingImports.push_back({imp, resolved});
            if (seen.insert(resolved).second) queue.push_back({resolved, imp->loc});
        }
        fileIds[path] = id;
    }
    // Now that every file has an id, point each import at its file (for `alias::name`).
    for (auto& [imp, resolved] : pendingImports) {
        auto it = fileIds.find(resolved);
        if (it != fileIds.end()) imp->targetFile = it->second;
    }
    if (diags.hasErrors()) return result;

    TypeContext types;
    Sema sema(program, arena, types, diags);
    if (!sema.run()) return result;

    std::set<std::string> dlls;
    for (FuncDecl* f : sema.externs) dlls.insert(f->externDll);
    result.dlls.assign(dlls.begin(), dlls.end());

    if (options.output.empty()) {
        result.ok = true;
        return result;
    }

    CodeGen codegen(sema, types);
    Image image = codegen.generate();
    std::vector<uint8_t> exe = writePE(image, options.subsystem);

    std::error_code ec;
    fs::path parent = fs::path(options.output).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    std::ofstream out(options.output, std::ios::binary);
    if (!out) {
        diags.error(SourceLoc{0, 0, 0, UINT32_MAX}, "cannot write '" + options.output + "'");
        return result;
    }
    out.write(reinterpret_cast<const char*>(exe.data()), static_cast<std::streamsize>(exe.size()));
    result.ok = out.good();
    return result;
}

} // namespace ron
