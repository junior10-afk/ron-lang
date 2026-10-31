// ronc — the Ron compiler.

#include "diagnostics.hpp"
#include "driver.hpp"
#include "lexer.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void printUsage() {
    std::fputs(
        "ronc - the Ron compiler\n"
        "\n"
        "Usage:\n"
        "  ronc build <file.ron> [-o out.exe] [--gui] [--release] [--Werror]\n"
        "                                               compile to a native executable\n"
        "  ronc run   <file.ron> [--gui] [--watch]      compile and run (--watch: rebuild and\n"
        "                                               restart whenever a source file changes)\n"
        "  ronc check <file.ron>                        type-check only\n"
        "  ronc tokens <file.ron>                       print the lexer's tokens\n"
        "  ronc test  <file.ron>                        run the file's test \"name\" { ... } blocks\n"
        "  ronc new   <folder>                          create a new project\n"
        "  ronc --version\n"
        "\n"
        "  ronc <file.ron> is a shortcut for 'ronc build <file.ron>'.\n"
        "\n"
        "  --release   no bounds checks (faster)    --Werror   treat warnings as errors\n",
        stderr);
}

std::string exeDir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return fs::path(std::string(buf, n)).parent_path().string();
}

std::string stdRoot() {
    if (const char* home = std::getenv("RON_HOME")) return home;
    return exeDir();
}

void printToken(const ron::Token& t) {
    std::printf("%4u:%-3u  %-14s", t.loc.line, t.loc.column, ron::tokenKindName(t.kind));
    switch (t.kind) {
    case ron::TokenKind::Identifier:
    case ron::TokenKind::StringLiteral:
        std::printf(" %.*s", static_cast<int>(t.text.size()), t.text.data());
        break;
    case ron::TokenKind::IntLiteral:
        std::printf(" %llu", static_cast<unsigned long long>(t.intValue));
        break;
    case ron::TokenKind::CharLiteral:
        std::printf(" %.*s (U+%04llX)", static_cast<int>(t.text.size()), t.text.data(),
                    static_cast<unsigned long long>(t.intValue));
        break;
    case ron::TokenKind::FloatLiteral:
        std::printf(" %g%s", t.floatValue, t.isF32 ? " (f32)" : "");
        break;
    default:
        break;
    }
    std::printf("\n");
}

int dumpTokens(const std::string& path) {
    ron::SourceManager sources;
    std::string text;
    if (!ron::readFile(path, text)) {
        std::fprintf(stderr, "ronc: cannot open '%s'\n", path.c_str());
        return 2;
    }
    uint32_t id = sources.add(path, std::move(text));
    ron::Diagnostics diags(sources);
    ron::Lexer lexer(sources.get(id), id, diags);
    for (const auto& t : lexer.tokenizeAll()) printToken(t);
    diags.printAll();
    return diags.hasErrors() ? 1 : 0;
}

// Copies DLLs shipped next to ronc (e.g. ron_engine.dll) next to the program.
void deployDlls(const std::vector<std::string>& dlls, const fs::path& outDir) {
    for (const std::string& dll : dlls) {
        fs::path src = fs::path(exeDir()) / dll;
        if (!fs::exists(src)) continue;
        fs::path dst = outDir / dll;
        std::error_code ec;
        if (!fs::exists(dst) || fs::last_write_time(dst, ec) < fs::last_write_time(src, ec))
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    }
}

struct BuildOutcome {
    bool ok = false;
    std::vector<std::string> sources;
};

bool g_release = false;
bool g_werror = false;
bool g_test = false;

BuildOutcome build(const std::string& input, const std::string& output, ron::Subsystem subsystem) {
    ron::SourceManager sources;
    ron::Diagnostics diags(sources);
    ron::CompileOptions opts;
    opts.input = input;
    opts.output = output;
    opts.stdRoot = stdRoot();
    opts.subsystem = subsystem;
    opts.release = g_release;
    opts.warningsAsErrors = g_werror;
    opts.testMode = g_test;
    ron::CompileResult r = ron::compile(opts, sources, diags);
    diags.printAll();
    if (!r.ok) {
        std::fprintf(stderr, "%zu error(s).\n", diags.errorCount());
        return {false, r.sourceFiles};
    }
    if (!output.empty()) deployDlls(r.dlls, fs::absolute(output).parent_path());
    return {true, r.sourceFiles};
}

PROCESS_INFORMATION launch(const std::string& exe) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + exe + "\"";
    std::string dir = fs::path(exe).parent_path().string();
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
        std::fprintf(stderr, "ronc: failed to start '%s' (error %lu)\n", exe.c_str(), GetLastError());
    return pi;
}

std::map<std::string, fs::file_time_type> snapshot(const std::vector<std::string>& files) {
    std::map<std::string, fs::file_time_type> m;
    for (const auto& f : files) {
        std::error_code ec;
        m[f] = fs::last_write_time(f, ec);
    }
    return m;
}

int newProject(const std::string& dir) {
    fs::path root(dir);
    std::error_code ec;
    if (fs::exists(root / "main.ron")) {
        std::fprintf(stderr, "ronc: %s already contains main.ron\n", dir.c_str());
        return 1;
    }
    fs::create_directories(root, ec);
    std::FILE* f = std::fopen((root / "main.ron").string().c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "ronc: cannot create %s\n", (root / "main.ron").string().c_str());
        return 1;
    }
    std::fputs("import \"std/io.ron\";\n"
               "\n"
               "fn greet(u8* name) -> String { return format(\"Hello, {}!\", name); }\n"
               "\n"
               "fn main() -> int {\n"
               "    println(greet(\"Ron\"));\n"
               "    return 0;\n"
               "}\n"
               "\n"
               "test \"greet builds the message\" {\n"
               "    assert(greet(\"you\") == \"Hello, you!\");\n"
               "}\n",
               f);
    std::fclose(f);
    if (std::FILE* g = std::fopen((root / ".gitignore").string().c_str(), "wb")) {
        std::fputs("*.exe\n*.dll\n", g);
        std::fclose(g);
    }
    std::string main = (root / "main.ron").string();
    std::printf("created %s\n  ronc run %s\n  ronc test %s\n", main.c_str(), main.c_str(), main.c_str());
    return 0;
}

int run(const std::string& input, ron::Subsystem subsystem, bool watch) {
    std::string output = (fs::path(input).replace_extension(g_test ? ".test.exe" : ".exe")).string();
    BuildOutcome b = build(input, output, subsystem);
    if (!watch) {
        if (!b.ok) return 1;
        PROCESS_INFORMATION pi = launch(fs::absolute(output).string());
        if (!pi.hProcess) return 1;
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return static_cast<int>(code);
    }

    // Live reload: rebuild and restart the program whenever a source file changes.
    std::fprintf(stderr, "ronc: watching %zu file(s) - press Ctrl+C to stop\n", b.sources.size());
    PROCESS_INFORMATION pi{};
    if (b.ok) pi = launch(fs::absolute(output).string());
    auto stamps = snapshot(b.sources);
    while (true) {
        Sleep(250);
        if (snapshot(b.sources) == stamps) continue;
        Sleep(100);   // let the editor finish writing
        if (pi.hProcess) {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 2000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            pi = {};
        }
        std::fprintf(stderr, "ronc: change detected, rebuilding...\n");
        BuildOutcome nb = build(input, output, subsystem);
        if (!nb.sources.empty()) b.sources = nb.sources;
        stamps = snapshot(b.sources);
        if (nb.ok) {
            std::fprintf(stderr, "ronc: restarted\n");
            pi = launch(fs::absolute(output).string());
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        printUsage();
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "--version" || args[0] == "-v") {
        std::printf("ronc %s\n", ron::kRonVersion);
        return 0;
    }
    if (args[0] == "new") {
        if (args.size() != 2) {
            printUsage();
            return 2;
        }
        return newProject(args[1]);
    }

    std::string command = "build";
    size_t i = 0;
    if (args[0] == "build" || args[0] == "run" || args[0] == "check" || args[0] == "tokens" || args[0] == "test") {
        command = args[0];
        i = 1;
    }

    std::string input, output;
    bool gui = false, watch = false;
    for (; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) output = args[++i];
        else if (args[i] == "--gui") gui = true;
        else if (args[i] == "--watch") watch = true;
        else if (args[i] == "--release") g_release = true;
        else if (args[i] == "--Werror") g_werror = true;
        else if (input.empty()) input = args[i];
        else {
            printUsage();
            return 2;
        }
    }
    if (input.empty()) {
        printUsage();
        return 2;
    }
    auto subsystem = gui ? ron::Subsystem::Windows : ron::Subsystem::Console;

    if (command == "tokens") return dumpTokens(input);
    if (command == "check") return build(input, "", subsystem).ok ? 0 : 1;
    if (command == "run") return run(input, subsystem, watch);
    if (command == "test") {
        g_test = true;
        return run(input, subsystem, false);
    }

    if (output.empty()) output = fs::path(input).replace_extension(".exe").string();
    if (!build(input, output, subsystem).ok) return 1;
    std::printf("%s -> %s\n", input.c_str(), output.c_str());
    return 0;
}
