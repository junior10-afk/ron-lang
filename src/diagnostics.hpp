#pragma once

#include "token.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ron {

enum class Severity { Error, Warning, Note };

struct Diagnostic {
    Severity severity;
    SourceLoc loc;
    std::string message;
};

// A source file loaded in memory.
struct SourceFile {
    std::string path;
    std::string text;

    std::string_view lineAt(uint32_t line) const;
};

// Owns every source file of a compilation; SourceLoc::fileId indexes into it.
class SourceManager {
public:
    uint32_t add(std::string path, std::string text);
    const SourceFile& get(uint32_t id) const { return *files_[id]; }
    size_t count() const { return files_.size(); }

private:
    std::vector<std::unique_ptr<SourceFile>> files_;
};

// Collects errors and prints them compiler-style:
//   game.ron:3:7: error: message
//       int x = @;
//               ^
class Diagnostics {
public:
    explicit Diagnostics(const SourceManager& sources) : sources_(sources) {}

    void error(SourceLoc loc, std::string message);
    void warning(SourceLoc loc, std::string message);
    void note(SourceLoc loc, std::string message);

    bool hasErrors() const { return errorCount_ > 0; }
    size_t errorCount() const { return errorCount_; }
    const std::vector<Diagnostic>& all() const { return diags_; }

    std::string format(const Diagnostic& d) const;
    void printAll() const;

private:
    const SourceManager& sources_;
    std::vector<Diagnostic> diags_;
    size_t errorCount_ = 0;
};

} // namespace ron
