#pragma once

#include "diagnostics.hpp"
#include "pe.hpp"

#include <string>
#include <vector>

namespace ron {

struct CompileOptions {
    std::string input;
    std::string output;            // .exe path; empty = check only
    std::string stdRoot;           // directory that contains `std/`
    Subsystem subsystem = Subsystem::Console;
    bool release = false;          // no bounds checks
    bool warningsAsErrors = false;
    bool testMode = false;         // build the test blocks with a generated main
};

constexpr const char* kRonVersion = "0.2.0";

struct CompileResult {
    bool ok = false;
    std::vector<std::string> sourceFiles;   // every file read (for --watch)
    std::vector<std::string> dlls;          // DLLs the program imports
};

// Full pipeline: load + imports → lex → parse → sema → codegen → PE.
CompileResult compile(const CompileOptions& options, SourceManager& sources, Diagnostics& diags);

bool readFile(const std::string& path, std::string& out);

} // namespace ron
