#include "gltf.hpp"

#include "jpeg.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace ron::gltf {

namespace {

// ───────────────────────────── JSON ─────────────────────────────

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    const Json* get(const char* key) const {
        for (const auto& [k, v] : members)
            if (k == key) return &v;
        return nullptr;
    }
    double num(const char* key, double fallback) const {
        const Json* v = get(key);
        return v && v->type == Type::Number ? v->number : fallback;
    }
    int integer(const char* key, int fallback) const { return static_cast<int>(num(key, fallback)); }
    std::string str(const char* key) const {
        const Json* v = get(key);
        return v && v->type == Type::String ? v->string : std::string();
    }
    size_t size() const { return items.size(); }
    const Json& operator[](size_t i) const { return items[i]; }
};

struct JsonParser {
    const char* p;
    const char* end;
    bool ok = true;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }

    std::string parseString() {
        std::string s;
        ++p;   // opening quote
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                case 'n': s += '\n'; break;
                case 't': s += '\t'; break;
                case 'r': s += '\r'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'u': {
                    if (p + 4 >= end) { ok = false; return s; }
                    unsigned cp = static_cast<unsigned>(std::strtoul(std::string(p + 1, 4).c_str(), nullptr, 16));
                    p += 4;
                    if (cp < 0x80) s += static_cast<char>(cp);
                    else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
                    else { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: s += *p; break;
                }
                ++p;
            } else {
                s += *p++;
            }
        }
        if (p >= end) ok = false;
        else ++p;   // closing quote
        return s;
    }

    Json parse(int depth = 0) {
        Json j;
        ws();
        if (p >= end || depth > 64) {
            ok = false;
            return j;
        }
        if (*p == '{') {
            j.type = Json::Type::Object;
            ++p;
            ws();
            if (p < end && *p == '}') { ++p; return j; }
            while (ok) {
                ws();
                if (p >= end || *p != '"') { ok = false; break; }
                std::string key = parseString();
                ws();
                if (p >= end || *p != ':') { ok = false; break; }
                ++p;
                j.members.emplace_back(std::move(key), parse(depth + 1));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; break; }
                ok = false;
            }
        } else if (*p == '[') {
            j.type = Json::Type::Array;
            ++p;
            ws();
            if (p < end && *p == ']') { ++p; return j; }
            while (ok) {
                j.items.push_back(parse(depth + 1));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; break; }
                ok = false;
            }
        } else if (*p == '"') {
            j.type = Json::Type::String;
            j.string = parseString();
        } else if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) {
            j.type = Json::Type::Bool;
            j.boolean = true;
            p += 4;
        } else if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) {
            j.type = Json::Type::Bool;
            p += 5;
        } else if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) {
            p += 4;
        } else {
            char* stop = nullptr;
            std::string tmp(p, std::min<size_t>(64, static_cast<size_t>(end - p)));
            j.number = std::strtod(tmp.c_str(), &stop);
            if (stop == tmp.c_str()) { ok = false; return j; }
            j.type = Json::Type::Number;
            p += stop - tmp.c_str();
        }
        return j;
    }
};

// ───────────────────────────── Helpers ─────────────────────────────

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

std::vector<uint8_t> base64Decode(const std::string& s) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v = value(c);
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return out;
}

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4 fromTRS(const float* t, const float* q, const float* s) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    Mat4 m{};
    m[0] = (1 - 2 * (y * y + z * z)) * s[0];
    m[1] = (2 * (x * y + z * w)) * s[0];
    m[2] = (2 * (x * z - y * w)) * s[0];
    m[4] = (2 * (x * y - z * w)) * s[1];
    m[5] = (1 - 2 * (x * x + z * z)) * s[1];
    m[6] = (2 * (y * z + x * w)) * s[1];
    m[8] = (2 * (x * z + y * w)) * s[2];
    m[9] = (2 * (y * z - x * w)) * s[2];
    m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1;
    return m;
}

void slerp(const float* a, const float* b, float t, float* out) {
    float bq[4] = {b[0], b[1], b[2], b[3]};
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) {   // take the short way around
        d = -d;
        for (float& v : bq) v = -v;
    }
    float wa, wb;
    if (d > 0.9995f) {
        wa = 1 - t;
        wb = t;
    } else {
        float theta = std::acos(d);
        float sn = std::sin(theta);
        wa = std::sin((1 - t) * theta) / sn;
        wb = std::sin(t * theta) / sn;
    }
    float len = 0;
    for (int i = 0; i < 4; ++i) {
        out[i] = wa * a[i] + wb * bq[i];
        len += out[i] * out[i];
    }
    len = std::sqrt(len);
    if (len > 0)
        for (int i = 0; i < 4; ++i) out[i] /= len;
}

struct Loader {
    const Json& doc;
    std::vector<std::vector<uint8_t>> buffers;
    std::string error;

    // Reads accessor `index` as floats (normalized integers are mapped to 0..1 / -1..1).
    bool floats(int index, std::vector<float>& out, int& components) {
        const Json* accessors = doc.get("accessors");
        if (!accessors || index < 0 || static_cast<size_t>(index) >= accessors->size()) return false;
        const Json& acc = (*accessors)[index];
        std::string type = acc.str("type");
        components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "MAT4" ? 16 : 0;
        int ctype = acc.integer("componentType", 5126);
        size_t count = static_cast<size_t>(acc.num("count", 0));
        bool normalized = acc.get("normalized") && acc.get("normalized")->boolean;
        out.assign(count * components, 0.0f);
        const Json* viewIndex = acc.get("bufferView");
        if (!viewIndex) return components > 0;   // all zeros (sparse accessors are not supported)
        const Json& view = (*doc.get("bufferViews"))[static_cast<size_t>(viewIndex->number)];
        size_t bufferIndex = static_cast<size_t>(view.num("buffer", 0));
        if (bufferIndex >= buffers.size()) return false;
        const std::vector<uint8_t>& buf = buffers[bufferIndex];
        size_t compSize = ctype == 5126 || ctype == 5125 ? 4 : ctype == 5122 || ctype == 5123 ? 2 : 1;
        size_t stride = static_cast<size_t>(view.num("byteStride", 0));
        if (stride == 0) stride = compSize * components;
        size_t base = static_cast<size_t>(view.num("byteOffset", 0)) + static_cast<size_t>(acc.num("byteOffset", 0));
        if (count > 0 && base + (count - 1) * stride + compSize * components > buf.size()) {
            error = "accessor reads past the end of its buffer";
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* src = buf.data() + base + i * stride;
            for (int c = 0; c < components; ++c) {
                const uint8_t* p = src + c * compSize;
                float v = 0;
                switch (ctype) {
                case 5126: std::memcpy(&v, p, 4); break;
                case 5125: { uint32_t u; std::memcpy(&u, p, 4); v = static_cast<float>(u); break; }
                case 5123: { uint16_t u; std::memcpy(&u, p, 2); v = normalized ? u / 65535.0f : u; break; }
                case 5122: { int16_t s; std::memcpy(&s, p, 2); v = normalized ? std::max(s / 32767.0f, -1.0f) : s; break; }
                case 5121: v = normalized ? *p / 255.0f : *p; break;
                case 5120: { int8_t s = static_cast<int8_t>(*p); v = normalized ? std::max(s / 127.0f, -1.0f) : s; break; }
                default: break;
                }
                out[i * components + c] = v;
            }
        }
        return true;
    }

    // Integer accessor (indices, joints): exact values.
    bool ints(int index, std::vector<uint32_t>& out, int& components) {
        const Json& acc = (*doc.get("accessors"))[static_cast<size_t>(index)];
        int ctype = acc.integer("componentType", 5125);
        std::vector<float> tmp;
        if (ctype == 5125) {
            // Read 32-bit values directly (floats would lose precision above 2^24).
            std::string type = acc.str("type");
            components = type == "SCALAR" ? 1 : type == "VEC4" ? 4 : 1;
            size_t count = static_cast<size_t>(acc.num("count", 0));
            const Json& view = (*doc.get("bufferViews"))[static_cast<size_t>(acc.num("bufferView", 0))];
            const std::vector<uint8_t>& buf = buffers[static_cast<size_t>(view.num("buffer", 0))];
            size_t stride = static_cast<size_t>(view.num("byteStride", 0));
            if (stride == 0) stride = 4u * components;
            size_t base = static_cast<size_t>(view.num("byteOffset", 0)) + static_cast<size_t>(acc.num("byteOffset", 0));
            if (count > 0 && base + (count - 1) * stride + 4u * components > buf.size()) return false;
            out.resize(count * components);
            for (size_t i = 0; i < count; ++i)
                for (int c = 0; c < components; ++c) std::memcpy(&out[i * components + c], buf.data() + base + i * stride + c * 4, 4);
            return true;
        }
        if (!floats(index, tmp, components)) return false;
        out.resize(tmp.size());
        for (size_t i = 0; i < tmp.size(); ++i) out[i] = static_cast<uint32_t>(tmp[i]);
        return true;
    }
};

} // namespace

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[col * 4 + k];
            r[col * 4 + row] = s;
        }
    return r;
}

bool loadFromMemory(const uint8_t* data, size_t size, const std::string& baseDir, Model& out, std::string& error) {
    std::string jsonText;
    std::vector<uint8_t> glbBin;
    if (size >= 12 && std::memcmp(data, "glTF", 4) == 0) {
        // GLB: header, JSON chunk, optional BIN chunk
        size_t pos = 12;
        while (pos + 8 <= size) {
            uint32_t len, type;
            std::memcpy(&len, data + pos, 4);
            std::memcpy(&type, data + pos + 4, 4);
            if (pos + 8 + len > size) break;
            if (type == 0x4E4F534A) jsonText.assign(reinterpret_cast<const char*>(data + pos + 8), len);
            else if (type == 0x004E4942) glbBin.assign(data + pos + 8, data + pos + 8 + len);
            pos += 8 + ((len + 3) & ~3u);
        }
    } else {
        jsonText.assign(reinterpret_cast<const char*>(data), size);
    }
    JsonParser parser{jsonText.data(), jsonText.data() + jsonText.size()};
    Json doc = parser.parse();
    if (!parser.ok || doc.type != Json::Type::Object) {
        error = "invalid glTF JSON";
        return false;
    }

    Loader L{doc, {}, {}};
    if (const Json* buffers = doc.get("buffers")) {
        for (size_t i = 0; i < buffers->size(); ++i) {
            std::string uri = (*buffers)[i].str("uri");
            std::vector<uint8_t> bytes;
            if (uri.empty()) {
                bytes = glbBin;
            } else if (uri.rfind("data:", 0) == 0) {
                size_t comma = uri.find(',');
                bytes = base64Decode(comma == std::string::npos ? std::string() : uri.substr(comma + 1));
            } else if (!readFile(baseDir + "/" + uri, bytes)) {
                error = "cannot read buffer '" + uri + "'";
                return false;
            }
            L.buffers.push_back(std::move(bytes));
        }
    }

    // Images (PNG only)
    if (const Json* images = doc.get("images")) {
        for (size_t i = 0; i < images->size(); ++i) {
            const Json& img = (*images)[i];
            std::vector<uint8_t> bytes;
            if (const Json* bv = img.get("bufferView")) {
                const Json& view = (*doc.get("bufferViews"))[static_cast<size_t>(bv->number)];
                const auto& buf = L.buffers[static_cast<size_t>(view.num("buffer", 0))];
                size_t off = static_cast<size_t>(view.num("byteOffset", 0));
                size_t len = static_cast<size_t>(view.num("byteLength", 0));
                if (off + len <= buf.size()) bytes.assign(buf.begin() + off, buf.begin() + off + len);
            } else {
                std::string uri = img.str("uri");
                if (uri.rfind("data:", 0) == 0) bytes = base64Decode(uri.substr(uri.find(',') + 1));
                else readFile(baseDir + "/" + uri, bytes);
            }
            png::Image decoded;
            std::string pngError;
            if (bytes.empty() || !decodeImage(bytes.data(), bytes.size(), decoded, pngError)) {
                // Undecodable image: fall back to a white pixel.
                decoded.width = decoded.height = 1;
                decoded.rgba = {255, 255, 255, 255};
            }
            out.images.push_back(std::move(decoded));
        }
    }

    // Materials (texture infos -> textures[] -> images[])
    if (const Json* materials = doc.get("materials")) {
        const Json* textures = doc.get("textures");
        auto imageOf = [&](const Json* info) -> int {
            if (!info || !textures) return -1;
            int t = info->integer("index", -1);
            if (t < 0 || static_cast<size_t>(t) >= textures->size()) return -1;
            int img = (*textures)[static_cast<size_t>(t)].integer("source", -1);
            return img >= 0 && static_cast<size_t>(img) < out.images.size() ? img : -1;
        };
        for (size_t i = 0; i < materials->size(); ++i) {
            const Json& mj = (*materials)[i];
            Material m;
            m.name = mj.str("name");
            if (const Json* pbr = mj.get("pbrMetallicRoughness")) {
                if (const Json* f = pbr->get("baseColorFactor"))
                    for (size_t c = 0; c < 4 && c < f->size(); ++c) m.baseColor[c] = static_cast<float>((*f)[c].number);
                m.roughness = static_cast<float>(pbr->num("roughnessFactor", 1.0));
                m.metallic = static_cast<float>(pbr->num("metallicFactor", 1.0));
                m.image = imageOf(pbr->get("baseColorTexture"));
                m.metallicRoughnessImage = imageOf(pbr->get("metallicRoughnessTexture"));
            } else {
                m.metallic = 0.0f;   // no PBR block: a plain dielectric
            }
            if (const Json* n = mj.get("normalTexture")) {
                m.normalImage = imageOf(n);
                m.normalScale = static_cast<float>(n->num("scale", 1.0));
            }
            m.occlusionImage = imageOf(mj.get("occlusionTexture"));
            m.emissiveImage = imageOf(mj.get("emissiveTexture"));
            if (const Json* e = mj.get("emissiveFactor"))
                for (size_t c = 0; c < 3 && c < e->size(); ++c) m.emissive[c] = static_cast<float>((*e)[c].number);
            if (const Json* ext = mj.get("extensions"))
                if (const Json* es = ext->get("KHR_materials_emissive_strength"))
                    for (float& c : m.emissive) c *= static_cast<float>(es->num("emissiveStrength", 1.0));
            std::string alpha = mj.str("alphaMode");
            m.alphaMode = alpha == "MASK" ? AlphaMode::Mask : alpha == "BLEND" ? AlphaMode::Blend : AlphaMode::Opaque;
            m.alphaCutoff = static_cast<float>(mj.num("alphaCutoff", 0.5));
            m.doubleSided = mj.get("doubleSided") && mj.get("doubleSided")->boolean;
            out.materials.push_back(m);
        }
    }

    // Meshes
    if (const Json* meshes = doc.get("meshes")) {
        for (size_t mi = 0; mi < meshes->size(); ++mi) {
            Mesh mesh;
            const Json& mj = (*meshes)[mi];
            if (const Json* w = mj.get("weights"))
                for (size_t k = 0; k < w->size(); ++k) mesh.weights.push_back(static_cast<float>((*w)[k].number));
            if (const Json* extras = mj.get("extras"))
                if (const Json* names = extras->get("targetNames"))
                    for (size_t k = 0; k < names->size(); ++k) mesh.targetNames.push_back((*names)[k].string);
            const Json* prims = mj.get("primitives");
            for (size_t pi = 0; prims && pi < prims->size(); ++pi) {
                const Json& prim = (*prims)[pi];
                if (prim.integer("mode", 4) != 4) continue;   // triangles only
                const Json* attrs = prim.get("attributes");
                if (!attrs || !attrs->get("POSITION")) continue;
                std::vector<float> pos, nrm, uv, wts, tan;
                std::vector<uint32_t> jts;
                int pc = 0, nc = 0, uc = 0, wc = 0, jc = 0, tc = 0;
                if (!L.floats(attrs->integer("POSITION", -1), pos, pc) || pc != 3) {
                    error = L.error.empty() ? "invalid POSITION accessor" : L.error;
                    return false;
                }
                size_t count = pos.size() / 3;
                bool hasNormals = attrs->get("NORMAL") && L.floats(attrs->integer("NORMAL", -1), nrm, nc) && nc == 3;
                bool hasUv = attrs->get("TEXCOORD_0") && L.floats(attrs->integer("TEXCOORD_0", -1), uv, uc) && uc == 2;
                bool hasTangents = hasNormals && attrs->get("TANGENT") &&
                                   L.floats(attrs->integer("TANGENT", -1), tan, tc) && tc == 4;
                bool hasSkin = attrs->get("JOINTS_0") && attrs->get("WEIGHTS_0") &&
                               L.ints(attrs->integer("JOINTS_0", -1), jts, jc) && jc == 4 &&
                               L.floats(attrs->integer("WEIGHTS_0", -1), wts, wc) && wc == 4;
                Primitive p;
                p.material = prim.integer("material", -1);
                p.skinned = hasSkin;
                p.vertices.resize(count);
                for (size_t v = 0; v < count; ++v) {
                    Vertex& vx = p.vertices[v];
                    std::memset(&vx, 0, sizeof(vx));
                    std::memcpy(vx.position, &pos[v * 3], 12);
                    if (hasNormals) std::memcpy(vx.normal, &nrm[v * 3], 12);
                    if (hasUv) std::memcpy(vx.uv, &uv[v * 2], 8);
                    if (hasTangents) std::memcpy(vx.tangent, &tan[v * 4], 16);
                    if (hasSkin) {
                        float total = 0;
                        for (int k = 0; k < 4; ++k) {
                            vx.joints[k] = static_cast<uint8_t>(std::min<uint32_t>(jts[v * 4 + k], 255));
                            vx.weights[k] = wts[v * 4 + k];
                            total += vx.weights[k];
                        }
                        if (total > 0)
                            for (float& w : vx.weights) w /= total;
                        else
                            vx.weights[0] = 1;
                    }
                }
                if (prim.get("indices")) {
                    int ic = 0;
                    if (!L.ints(prim.integer("indices", -1), p.indices, ic)) {
                        error = "invalid index accessor";
                        return false;
                    }
                } else {
                    for (uint32_t v = 0; v < count; ++v) p.indices.push_back(v);
                }
                for (uint32_t& idx : p.indices)
                    if (idx >= count) idx = 0;
                p.indices.resize(p.indices.size() / 3 * 3);
                if (!hasNormals) computeNormals(p.vertices, p.indices);
                if (!hasTangents) computeTangents(p.vertices, p.indices);
                // Morph targets (POSITION / NORMAL deltas)
                if (const Json* targets = prim.get("targets")) {
                    for (size_t ti = 0; ti < targets->size(); ++ti) {
                        const Json& t = (*targets)[ti];
                        MorphTarget mt;
                        int c = 0;
                        if (!t.get("POSITION") || !L.floats(t.integer("POSITION", -1), mt.positions, c) || c != 3 ||
                            mt.positions.size() != count * 3)
                            mt.positions.assign(count * 3, 0.0f);
                        if (t.get("NORMAL") && !(L.floats(t.integer("NORMAL", -1), mt.normals, c) && c == 3 &&
                                                 mt.normals.size() == count * 3))
                            mt.normals.clear();
                        p.targets.push_back(std::move(mt));
                    }
                }
                mesh.primitives.push_back(std::move(p));
            }
            out.meshes.push_back(std::move(mesh));
        }
    }

    // Nodes
    if (const Json* nodes = doc.get("nodes")) {
        out.nodes.resize(nodes->size());
        for (size_t i = 0; i < nodes->size(); ++i) {
            const Json& n = (*nodes)[i];
            Node& node = out.nodes[i];
            node.name = n.str("name");
            node.mesh = n.integer("mesh", -1);
            node.skin = n.integer("skin", -1);
            if (const Json* t = n.get("translation"))
                for (size_t c = 0; c < 3 && c < t->size(); ++c) node.translation[c] = static_cast<float>((*t)[c].number);
            if (const Json* r = n.get("rotation"))
                for (size_t c = 0; c < 4 && c < r->size(); ++c) node.rotation[c] = static_cast<float>((*r)[c].number);
            if (const Json* s = n.get("scale"))
                for (size_t c = 0; c < 3 && c < s->size(); ++c) node.scale[c] = static_cast<float>((*s)[c].number);
            if (const Json* m = n.get("matrix")) {
                node.hasMatrix = m->size() == 16;
                for (size_t c = 0; c < 16 && c < m->size(); ++c) node.matrix[c] = static_cast<float>((*m)[c].number);
            }
            if (const Json* ch = n.get("children"))
                for (size_t c = 0; c < ch->size(); ++c) node.children.push_back(static_cast<int>((*ch)[c].number));
            if (const Json* w = n.get("weights"))
                for (size_t c = 0; c < w->size(); ++c) node.weights.push_back(static_cast<float>((*w)[c].number));
        }
        for (size_t i = 0; i < out.nodes.size(); ++i)
            for (int c : out.nodes[i].children)
                if (c >= 0 && static_cast<size_t>(c) < out.nodes.size()) out.nodes[c].parent = static_cast<int>(i);
        for (size_t i = 0; i < out.nodes.size(); ++i)
            if (out.nodes[i].parent < 0) out.roots.push_back(static_cast<int>(i));
    }

    // Skins
    if (const Json* skins = doc.get("skins")) {
        for (size_t i = 0; i < skins->size(); ++i) {
            const Json& s = (*skins)[i];
            Skin skin;
            if (const Json* j = s.get("joints"))
                for (size_t k = 0; k < j->size(); ++k) skin.joints.push_back(static_cast<int>((*j)[k].number));
            skin.inverseBind.assign(skin.joints.size(), identity());
            if (s.get("inverseBindMatrices")) {
                std::vector<float> m;
                int c = 0;
                if (L.floats(s.integer("inverseBindMatrices", -1), m, c) && c == 16)
                    for (size_t k = 0; k < skin.joints.size() && (k + 1) * 16 <= m.size(); ++k)
                        std::memcpy(skin.inverseBind[k].data(), &m[k * 16], 64);
            }
            out.skins.push_back(std::move(skin));
        }
    }

    // Animations
    if (const Json* anims = doc.get("animations")) {
        for (size_t i = 0; i < anims->size(); ++i) {
            const Json& a = (*anims)[i];
            Animation anim;
            anim.name = a.str("name");
            const Json* samplers = a.get("samplers");
            const Json* channels = a.get("channels");
            for (size_t c = 0; channels && samplers && c < channels->size(); ++c) {
                const Json& ch = (*channels)[c];
                const Json* target = ch.get("target");
                int si = ch.integer("sampler", -1);
                if (!target || si < 0 || static_cast<size_t>(si) >= samplers->size()) continue;
                std::string path = target->str("path");
                if (path != "translation" && path != "rotation" && path != "scale" && path != "weights") continue;
                const Json& sampler = (*samplers)[static_cast<size_t>(si)];
                Channel channel;
                channel.node = target->integer("node", -1);
                channel.path = path == "translation" ? Path::Translation
                             : path == "rotation"    ? Path::Rotation
                             : path == "scale"       ? Path::Scale
                                                     : Path::Weights;
                std::string interp = sampler.str("interpolation");
                channel.step = interp == "STEP";
                int ic = 0, oc = 0;
                if (!L.floats(sampler.integer("input", -1), channel.times, ic) ||
                    !L.floats(sampler.integer("output", -1), channel.values, oc))
                    continue;
                if (channel.times.empty()) continue;
                // Elements per key: 3/4 for TRS, the morph target count for weights.
                oc = static_cast<int>(channel.values.size() / channel.times.size() / (interp == "CUBICSPLINE" ? 3 : 1));
                if (oc <= 0) continue;
                if (interp == "CUBICSPLINE") {
                    // Keep only the value of each (in-tangent, value, out-tangent) triple.
                    std::vector<float> v;
                    for (size_t k = 0; k < channel.times.size(); ++k)
                        for (int e = 0; e < oc; ++e) v.push_back(channel.values[(k * 3 + 1) * oc + e]);
                    channel.values = std::move(v);
                }
                if (!channel.times.empty()) anim.duration = std::max(anim.duration, channel.times.back());
                anim.channels.push_back(std::move(channel));
            }
            out.animations.push_back(std::move(anim));
        }
    }
    return true;
}

bool load(const std::string& path, Model& out, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) {
        error = "cannot open '" + path + "'";
        return false;
    }
    size_t slash = path.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
    return loadFromMemory(bytes.data(), bytes.size(), dir, out, error);
}

namespace {

struct TRS {
    float t[3], r[4], s[3];
};

void sampleInto(const Model& m, int anim, float time, std::vector<TRS>& pose) {
    if (anim < 0 || static_cast<size_t>(anim) >= m.animations.size()) return;
    const Animation& a = m.animations[anim];
    if (a.duration > 0) time = std::fmod(std::max(time, 0.0f), a.duration);
    for (const Channel& ch : a.channels) {
        if (ch.node < 0 || static_cast<size_t>(ch.node) >= pose.size() || ch.times.empty()) continue;
        if (ch.path == Path::Weights) continue;   // see sampleWeights
        int n = ch.path == Path::Rotation ? 4 : 3;
        size_t keys = ch.times.size();
        size_t k1 = 0;
        while (k1 < keys && ch.times[k1] < time) ++k1;
        size_t k0 = k1 == 0 ? 0 : k1 - 1;
        if (k1 >= keys) k1 = keys - 1;
        float t = 0;
        if (k1 != k0 && !ch.step) t = (time - ch.times[k0]) / std::max(ch.times[k1] - ch.times[k0], 1e-6f);
        t = std::clamp(t, 0.0f, 1.0f);
        const float* a0 = &ch.values[k0 * n];
        const float* a1 = &ch.values[k1 * n];
        if (k1 * n + n > ch.values.size()) continue;
        TRS& p = pose[ch.node];
        if (ch.path == Path::Rotation) {
            slerp(a0, a1, t, p.r);
        } else {
            float* dst = ch.path == Path::Translation ? p.t : p.s;
            for (int i = 0; i < 3; ++i) dst[i] = a0[i] + (a1[i] - a0[i]) * t;
        }
    }
}

} // namespace

namespace {

// Overwrites `w` with the weights channel of `anim` targeting `node`, if any.
void sampleWeightsInto(const Model& m, int node, int anim, float time, std::vector<float>& w) {
    if (anim < 0 || static_cast<size_t>(anim) >= m.animations.size()) return;
    const Animation& a = m.animations[anim];
    if (a.duration > 0) time = std::fmod(std::max(time, 0.0f), a.duration);
    for (const Channel& ch : a.channels) {
        if (ch.node != node || ch.path != Path::Weights || ch.times.empty()) continue;
        size_t keys = ch.times.size();
        size_t n = ch.values.size() / keys;
        size_t k1 = 0;
        while (k1 < keys && ch.times[k1] < time) ++k1;
        size_t k0 = k1 == 0 ? 0 : k1 - 1;
        if (k1 >= keys) k1 = keys - 1;
        float t = 0;
        if (k1 != k0 && !ch.step) t = std::clamp((time - ch.times[k0]) / std::max(ch.times[k1] - ch.times[k0], 1e-6f), 0.0f, 1.0f);
        for (size_t i = 0; i < n && i < w.size(); ++i)
            w[i] = ch.values[k0 * n + i] + (ch.values[k1 * n + i] - ch.values[k0 * n + i]) * t;
    }
}

} // namespace

std::vector<float> sampleWeights(const Model& m, int node, int anim, float time, int animB, float timeB, float weight) {
    std::vector<float> base;
    if (node < 0 || static_cast<size_t>(node) >= m.nodes.size()) return base;
    const Node& n = m.nodes[node];
    if (n.mesh < 0 || static_cast<size_t>(n.mesh) >= m.meshes.size()) return base;
    const Mesh& mesh = m.meshes[n.mesh];
    size_t targets = 0;
    for (const Primitive& p : mesh.primitives) targets = std::max(targets, p.targets.size());
    base.assign(targets, 0.0f);
    const std::vector<float>& defaults = !n.weights.empty() ? n.weights : mesh.weights;
    for (size_t i = 0; i < targets && i < defaults.size(); ++i) base[i] = defaults[i];
    std::vector<float> a = base;
    sampleWeightsInto(m, node, anim, time, a);
    if (animB >= 0 && weight > 0) {
        std::vector<float> b = base;
        sampleWeightsInto(m, node, animB, timeB, b);
        float w = std::clamp(weight, 0.0f, 1.0f);
        for (size_t i = 0; i < a.size(); ++i) a[i] += (b[i] - a[i]) * w;
    }
    return a;
}

void computeNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    // Area-weighted face normals accumulated on vertices sharing a position, so
    // UV seams (duplicated vertices) do not show as creases.
    for (Vertex& v : vertices) v.normal[0] = v.normal[1] = v.normal[2] = 0;
    std::vector<float> acc(vertices.size() * 3, 0.0f);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const float* a = vertices[indices[i]].position;
        const float* b = vertices[indices[i + 1]].position;
        const float* c = vertices[indices[i + 2]].position;
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j) acc[indices[i + k] * 3 + j] += n[j];
    }
    // Merge vertices with identical positions (sort by position).
    std::vector<uint32_t> order(vertices.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    auto less = [&](uint32_t x, uint32_t y) {
        return std::lexicographical_compare(vertices[x].position, vertices[x].position + 3,
                                            vertices[y].position, vertices[y].position + 3);
    };
    std::sort(order.begin(), order.end(), less);
    for (size_t s = 0; s < order.size();) {
        size_t e = s + 1;
        while (e < order.size() && !less(order[s], order[e])) ++e;
        float sum[3] = {0, 0, 0};
        for (size_t k = s; k < e; ++k)
            for (int j = 0; j < 3; ++j) sum[j] += acc[order[k] * 3 + j];
        float len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
        for (size_t k = s; k < e; ++k) {
            Vertex& v = vertices[order[k]];
            if (len > 1e-20f)
                for (int j = 0; j < 3; ++j) v.normal[j] = sum[j] / len;
            else
                v.normal[1] = 1;
        }
        s = e;
    }
}

void computeTangents(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    // Per-triangle tangents from UV derivatives (Lengyel), accumulated per vertex,
    // then Gram-Schmidt orthogonalized against the normal.
    std::vector<float> tan(vertices.size() * 3, 0.0f), bit(vertices.size() * 3, 0.0f);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const Vertex& a = vertices[indices[i]];
        const Vertex& b = vertices[indices[i + 1]];
        const Vertex& c = vertices[indices[i + 2]];
        float e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
        float e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
        float du1 = b.uv[0] - a.uv[0], dv1 = b.uv[1] - a.uv[1];
        float du2 = c.uv[0] - a.uv[0], dv2 = c.uv[1] - a.uv[1];
        float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-12f) continue;
        float r = 1.0f / det;
        float t[3], bt[3];
        for (int j = 0; j < 3; ++j) {
            t[j] = (e1[j] * dv2 - e2[j] * dv1) * r;
            bt[j] = (e2[j] * du1 - e1[j] * du2) * r;
        }
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j) {
                tan[indices[i + k] * 3 + j] += t[j];
                bit[indices[i + k] * 3 + j] += bt[j];
            }
    }
    for (size_t v = 0; v < vertices.size(); ++v) {
        Vertex& vx = vertices[v];
        const float* n = vx.normal;
        float* t = &tan[v * 3];
        float d = n[0] * t[0] + n[1] * t[1] + n[2] * t[2];
        float o[3] = {t[0] - n[0] * d, t[1] - n[1] * d, t[2] - n[2] * d};
        float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (len < 1e-12f) {
            // No usable UVs: any vector perpendicular to the normal.
            float ref[3] = {std::fabs(n[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
            o[0] = ref[1] * n[2] - ref[2] * n[1];
            o[1] = ref[2] * n[0] - ref[0] * n[2];
            o[2] = ref[0] * n[1] - ref[1] * n[0];
            len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            if (len < 1e-12f) { o[0] = 1; o[1] = o[2] = 0; len = 1; }
        }
        for (int j = 0; j < 3; ++j) vx.tangent[j] = o[j] / len;
        // Handedness: does cross(n, t) point along the accumulated bitangent?
        float c[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
        const float* b = &bit[v * 3];
        vx.tangent[3] = c[0] * b[0] + c[1] * b[1] + c[2] * b[2] < 0 ? -1.0f : 1.0f;
    }
}

void samplePose(const Model& m, int anim, float time, int animB, float timeB, float weight, std::vector<Mat4>& localOut) {
    std::vector<TRS> base(m.nodes.size());
    for (size_t i = 0; i < m.nodes.size(); ++i) {
        const Node& n = m.nodes[i];
        std::memcpy(base[i].t, n.translation, 12);
        std::memcpy(base[i].r, n.rotation, 16);
        std::memcpy(base[i].s, n.scale, 12);
    }
    std::vector<TRS> a = base;
    sampleInto(m, anim, time, a);
    if (animB >= 0 && weight > 0) {
        std::vector<TRS> b = base;
        sampleInto(m, animB, timeB, b);
        float w = std::clamp(weight, 0.0f, 1.0f);
        for (size_t i = 0; i < a.size(); ++i) {
            for (int c = 0; c < 3; ++c) {
                a[i].t[c] += (b[i].t[c] - a[i].t[c]) * w;
                a[i].s[c] += (b[i].s[c] - a[i].s[c]) * w;
            }
            float r[4];
            slerp(a[i].r, b[i].r, w, r);
            std::memcpy(a[i].r, r, 16);
        }
    }
    localOut.resize(m.nodes.size());
    // Nodes given as a matrix cannot be animated (glTF rule).
    for (size_t i = 0; i < m.nodes.size(); ++i)
        localOut[i] = m.nodes[i].hasMatrix ? m.nodes[i].matrix : fromTRS(a[i].t, a[i].r, a[i].s);
}

void globalTransforms(const Model& m, const std::vector<Mat4>& local, std::vector<Mat4>& globalOut) {
    globalOut.assign(m.nodes.size(), identity());
    // Iterative depth-first traversal from the roots
    std::vector<int> stack(m.roots.rbegin(), m.roots.rend());
    std::vector<bool> done(m.nodes.size(), false);
    while (!stack.empty()) {
        int n = stack.back();
        stack.pop_back();
        if (n < 0 || static_cast<size_t>(n) >= m.nodes.size() || done[n]) continue;
        done[n] = true;
        int p = m.nodes[n].parent;
        globalOut[n] = p >= 0 ? multiply(globalOut[p], local[n]) : local[n];
        for (auto it = m.nodes[n].children.rbegin(); it != m.nodes[n].children.rend(); ++it) stack.push_back(*it);
    }
}

} // namespace ron::gltf
