#include "pe.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace ron {

namespace {

constexpr uint64_t kImageBase = 0x140000000ull;
constexpr uint32_t kSectionAlign = 0x1000;
constexpr uint32_t kFileAlign = 0x200;
constexpr uint32_t kHeadersSize = 0x400;

uint32_t alignTo(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

struct Writer {
    std::vector<uint8_t>& out;
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void bytes(const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    void padTo(size_t size) { out.resize(std::max(out.size(), size), 0); }
};

void put32(std::vector<uint8_t>& buf, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) buf[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
void put64(std::vector<uint8_t>& buf, size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i) buf[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// Builds the .idata section contents for a given section RVA.
struct ImportTables {
    std::vector<uint8_t> bytes;
    uint32_t descriptorsSize = 0;
    uint32_t iatOffset = 0;
    uint32_t iatSize = 0;
    std::vector<uint32_t> slotOffset;   // per Image import slot: offset of its IAT entry
};

ImportTables buildImports(const Image& image, uint32_t rva) {
    // Group slots by DLL, keeping first-seen order.
    std::vector<std::string> dlls;
    std::map<std::string, std::vector<size_t>> byDll;
    for (size_t i = 0; i < image.importSlots.size(); ++i) {
        const auto& s = image.importSlots[i];
        if (!byDll.count(s.dll)) dlls.push_back(s.dll);
        byDll[s.dll].push_back(i);
    }

    ImportTables t;
    t.slotOffset.resize(image.importSlots.size());
    uint32_t off = static_cast<uint32_t>((dlls.size() + 1) * 20);
    t.descriptorsSize = off;

    std::map<std::string, uint32_t> iltOff, iatOff;
    for (auto& d : dlls) {
        iltOff[d] = off;
        off += static_cast<uint32_t>((byDll[d].size() + 1) * 8);
    }
    t.iatOffset = off;
    for (auto& d : dlls) {
        iatOff[d] = off;
        for (size_t k = 0; k < byDll[d].size(); ++k) t.slotOffset[byDll[d][k]] = off + static_cast<uint32_t>(k * 8);
        off += static_cast<uint32_t>((byDll[d].size() + 1) * 8);
    }
    t.iatSize = off - t.iatOffset;

    std::vector<uint32_t> hintName(image.importSlots.size());
    for (size_t i = 0; i < image.importSlots.size(); ++i) {
        hintName[i] = off;
        off += static_cast<uint32_t>(2 + image.importSlots[i].name.size() + 1);
        off = alignTo(off, 2);
    }
    std::map<std::string, uint32_t> nameOff;
    for (auto& d : dlls) {
        nameOff[d] = off;
        off += static_cast<uint32_t>(d.size() + 1);
    }

    t.bytes.assign(alignTo(off, 8), 0);
    for (size_t di = 0; di < dlls.size(); ++di) {
        const std::string& d = dlls[di];
        size_t desc = di * 20;
        put32(t.bytes, desc + 0, rva + iltOff[d]);    // OriginalFirstThunk (ILT)
        put32(t.bytes, desc + 12, rva + nameOff[d]);  // Name
        put32(t.bytes, desc + 16, rva + iatOff[d]);   // FirstThunk (IAT)
        for (size_t k = 0; k < byDll[d].size(); ++k) {
            uint64_t entry = rva + hintName[byDll[d][k]];
            put64(t.bytes, iltOff[d] + k * 8, entry);
            put64(t.bytes, iatOff[d] + k * 8, entry);
        }
        std::memcpy(&t.bytes[nameOff[d]], d.c_str(), d.size());
    }
    for (size_t i = 0; i < image.importSlots.size(); ++i) {
        const std::string& n = image.importSlots[i].name;
        std::memcpy(&t.bytes[hintName[i] + 2], n.c_str(), n.size());
    }
    return t;
}

} // namespace

std::vector<uint8_t> writePE(const Image& image, Subsystem subsystem) {
    struct Sec {
        const char* name;
        std::vector<uint8_t> data;
        uint32_t characteristics;
        uint32_t rva = 0;
        uint32_t fileOffset = 0;
    };

    auto nonEmpty = [](std::vector<uint8_t> v) {
        if (v.empty()) v.push_back(0);
        return v;
    };

    Sec text{".text", image.text, 0x60000020};    // code | execute | read
    Sec rdata{".rdata", nonEmpty(image.rdata), 0x40000040};   // initialized | read
    Sec data{".data", nonEmpty(image.data), 0xC0000040};      // initialized | read | write
    Sec idata{".idata", {}, 0xC0000040};

    text.rva = kSectionAlign;
    rdata.rva = alignTo(text.rva + static_cast<uint32_t>(text.data.size()), kSectionAlign);
    data.rva = alignTo(rdata.rva + static_cast<uint32_t>(rdata.data.size()), kSectionAlign);
    idata.rva = alignTo(data.rva + static_cast<uint32_t>(data.data.size()), kSectionAlign);
    ImportTables imports = buildImports(image, idata.rva);
    idata.data = imports.bytes;
    uint32_t imageSize = alignTo(idata.rva + static_cast<uint32_t>(idata.data.size()), kSectionAlign);

    // Resolve RIP-relative fixups.
    for (const x64::Fixup& f : image.fixups) {
        uint32_t target = 0;
        switch (f.section) {
        case x64::Section::Text: target = text.rva + f.target; break;
        case x64::Section::Rdata: target = rdata.rva + f.target; break;
        case x64::Section::Data: target = data.rva + f.target; break;
        case x64::Section::Import: target = idata.rva + imports.slotOffset[f.target]; break;
        }
        uint32_t next = text.rva + f.at + 4;
        put32(text.data, f.at, target - next);
    }

    // Absolute addresses inside .rdata / .data
    for (const Image::AbsFixup& f : image.absFixups) {
        auto rvaOf = [&](x64::Section s) {
            return s == x64::Section::Text ? text.rva : s == x64::Section::Rdata ? rdata.rva
                 : s == x64::Section::Data ? data.rva : idata.rva;
        };
        std::vector<uint8_t>& bytes = f.where == x64::Section::Rdata ? rdata.data : data.data;
        put64(bytes, f.at, kImageBase + rvaOf(f.target) + f.offset);
    }

    Sec* sections[] = {&text, &rdata, &data, &idata};
    uint32_t fileOff = kHeadersSize;
    for (Sec* s : sections) {
        s->fileOffset = fileOff;
        fileOff += alignTo(static_cast<uint32_t>(s->data.size()), kFileAlign);
    }

    std::vector<uint8_t> out;
    Writer w{out};

    // DOS header: only e_magic and e_lfanew matter.
    w.u16(0x5A4D);   // "MZ"
    w.padTo(0x3C);
    w.u32(0x40);     // e_lfanew
    // PE signature + COFF header
    w.u32(0x00004550);   // "PE\0\0"
    w.u16(0x8664);       // AMD64
    w.u16(4);            // number of sections
    w.u32(0);            // timestamp
    w.u32(0);
    w.u32(0);
    w.u16(240);          // size of optional header
    w.u16(0x0023);       // RELOCS_STRIPPED | EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE

    // Optional header (PE32+)
    w.u16(0x20B);
    w.u8(1);             // linker version (ronc 0.1)
    w.u8(0);
    w.u32(alignTo(static_cast<uint32_t>(text.data.size()), kFileAlign));   // SizeOfCode
    w.u32(alignTo(static_cast<uint32_t>(rdata.data.size() + data.data.size() + idata.data.size()), kFileAlign));
    w.u32(0);            // SizeOfUninitializedData
    w.u32(text.rva + image.entryOffset);
    w.u32(text.rva);     // BaseOfCode
    w.u64(kImageBase);
    w.u32(kSectionAlign);
    w.u32(kFileAlign);
    w.u16(6); w.u16(0);  // OS version
    w.u16(0); w.u16(0);  // image version
    w.u16(6); w.u16(0);  // subsystem version
    w.u32(0);            // Win32VersionValue
    w.u32(imageSize);
    w.u32(kHeadersSize);
    w.u32(0);            // checksum
    w.u16(subsystem == Subsystem::Windows ? 2 : 3);
    w.u16(0x8100);       // NX_COMPAT | TERMINAL_SERVER_AWARE (no ASLR: no relocations)
    w.u64(8 * 1024 * 1024);   // stack reserve
    w.u64(64 * 1024);         // stack commit
    w.u64(1024 * 1024);       // heap reserve
    w.u64(4096);              // heap commit
    w.u32(0);                 // loader flags
    w.u32(16);                // number of data directories
    for (int i = 0; i < 16; ++i) {
        if (i == 1) {        // import directory
            w.u32(idata.rva);
            w.u32(imports.descriptorsSize);
        } else if (i == 12) {   // import address table
            w.u32(idata.rva + imports.iatOffset);
            w.u32(imports.iatSize);
        } else {
            w.u32(0);
            w.u32(0);
        }
    }

    // Section headers
    for (Sec* s : sections) {
        char name[8] = {};
        std::memcpy(name, s->name, std::strlen(s->name));
        w.bytes(name, 8);
        w.u32(static_cast<uint32_t>(s->data.size()));                        // VirtualSize
        w.u32(s->rva);
        w.u32(alignTo(static_cast<uint32_t>(s->data.size()), kFileAlign));   // SizeOfRawData
        w.u32(s->fileOffset);
        w.u32(0); w.u32(0); w.u16(0); w.u16(0);
        w.u32(s->characteristics);
    }

    w.padTo(kHeadersSize);
    for (Sec* s : sections) {
        w.padTo(s->fileOffset);
        w.bytes(s->data.data(), s->data.size());
    }
    w.padTo(fileOff);
    return out;
}

} // namespace ron
