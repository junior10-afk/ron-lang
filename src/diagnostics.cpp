#include "diagnostics.hpp"

#include <cstdio>

namespace ron {

std::string_view SourceFile::lineAt(uint32_t line) const {
    size_t start = 0;
    for (uint32_t l = 1; l < line; ++l) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) return {};
        start = nl + 1;
    }
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    if (end > start && text[end - 1] == '\r') --end;
    return std::string_view(text).substr(start, end - start);
}

uint32_t SourceManager::add(std::string path, std::string text) {
    files_.push_back(std::make_unique<SourceFile>(SourceFile{std::move(path), std::move(text)}));
    return static_cast<uint32_t>(files_.size() - 1);
}

void Diagnostics::error(SourceLoc loc, std::string message) {
    diags_.push_back({Severity::Error, loc, std::move(message)});
    ++errorCount_;
}

void Diagnostics::warning(SourceLoc loc, std::string message) {
    diags_.push_back({Severity::Warning, loc, std::move(message)});
}

void Diagnostics::note(SourceLoc loc, std::string message) {
    diags_.push_back({Severity::Note, loc, std::move(message)});
}

std::string Diagnostics::format(const Diagnostic& d) const {
    const char* label = d.severity == Severity::Error   ? "error"
                      : d.severity == Severity::Warning ? "warning"
                                                        : "note";
    if (d.loc.fileId >= sources_.count()) return std::string(label) + ": " + d.message + "\n";

    const SourceFile& file = sources_.get(d.loc.fileId);
    std::string out = file.path + ":" + std::to_string(d.loc.line) + ":" +
                      std::to_string(d.loc.column) + ": " + label + ": " + d.message + "\n";

    std::string_view line = file.lineAt(d.loc.line);
    out += "    ";
    out += line;
    out += "\n    ";
    // Keep tabs in the caret line so it stays aligned.
    for (uint32_t i = 1; i < d.loc.column && i - 1 < line.size(); ++i)
        out += line[i - 1] == '\t' ? '\t' : ' ';
    out += "^\n";
    return out;
}

void Diagnostics::printAll() const {
    for (const auto& d : diags_) std::fputs(format(d).c_str(), stderr);
}

} // namespace ron
