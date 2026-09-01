#pragma once

// Pack Merger — pure, host-testable merge engine (see packmerger.cpp for the
// module plumbing that runs this inside Minecraft).
//
// Minecraft Bedrock resolves resource-pack files by walking the pack stack
// from the highest-priority pack down; the first pack that contains a path
// wins and the remaining packs are never consulted for that path. Two packs
// that both ship `entity/player.entity.json` (e.g. a Java-animations pack and
// a Cape-Physics pack) therefore can never coexist: only the file of the
// higher pack is loaded and the other pack's bindings silently disappear.
//
// This engine automates the manual fix: for every file path that exists in
// MORE THAN ONE participating pack it produces a merged copy:
//
//   * `.json` files are deep-merged. The highest-priority pack wins every
//     conflicting key (exactly what the game would have loaded), and every
//     key that only exists in a lower pack is copied in recursively. Arrays
//     under known "list" keys (`scripts` -> `animate`) are unioned so the
//     animations of both packs get bound, not just one side's.
//   * `.lang` files are merged per key: the top pack's file is kept verbatim
//     and keys that only exist in a lower pack are appended.
//   * any other file (textures, sounds, ...) needs no entry — the stack
//     already lets the top pack win, which is the expected behavior.
//
// The merged copies are written into a small generated pack
// (`resource_packs/bedrocktoolsplus-merged/`) that sits on top of the stack;
// every non-conflicting path still resolves through the user's own packs.
//
// Bedrock JSON is not always strict JSON: files in the wild contain a UTF-8
// BOM, `//` and `/* */` comments and trailing commas. The parser below
// tolerates all three so one sloppy pack cannot break the merge of the
// others; a file that still fails to parse falls back to "copy the top
// pack's bytes" which reproduces the vanilla (unmerged) behavior for it.
//
// This header is self-contained (standard library only, no nlohmann, no
// game headers) so tests/packmerger_merge_test.cpp can run it on any host
// with a plain C++20 compiler — the same code packmerger.cpp executes
// in-game.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace bedrocktools::modules::packmerger {

// ---------------------------------------------------------------------------
// Minimal JSON value tree (insertion-ordered objects, like Bedrock writes)
// ---------------------------------------------------------------------------

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::vector<std::pair<std::string, JsonValue>> objectValue;

    bool isNull() const { return type == Type::Null; }
    bool isBool() const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    static JsonValue makeBool(bool v) {
        JsonValue j; j.type = Type::Bool; j.boolValue = v; return j;
    }
    static JsonValue makeNumber(double v) {
        JsonValue j; j.type = Type::Number; j.numberValue = v; return j;
    }
    static JsonValue makeString(std::string v) {
        JsonValue j; j.type = Type::String; j.stringValue = std::move(v); return j;
    }
    static JsonValue makeArray() {
        JsonValue j; j.type = Type::Array; return j;
    }
    static JsonValue makeObject() {
        JsonValue j; j.type = Type::Object; return j;
    }

    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : objectValue) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    JsonValue* find(const std::string& key) {
        if (type != Type::Object) return nullptr;
        for (auto& kv : objectValue) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    // Replaces an existing key or appends a new one (order preserved).
    void set(const std::string& key, JsonValue value) {
        if (type != Type::Object) {
            *this = makeObject();
        }
        for (auto& kv : objectValue) {
            if (kv.first == key) {
                kv.second = std::move(value);
                return;
            }
        }
        objectValue.emplace_back(key, std::move(value));
    }

    void push(JsonValue value) {
        if (type != Type::Array) {
            *this = makeArray();
        }
        arrayValue.push_back(std::move(value));
    }

    // Structural equality (used to de-duplicate unioned array entries).
    bool sameAs(const JsonValue& other) const {
        if (type != other.type) return false;
        switch (type) {
            case Type::Null: return true;
            case Type::Bool: return boolValue == other.boolValue;
            case Type::Number: return numberValue == other.numberValue;
            case Type::String: return stringValue == other.stringValue;
            case Type::Array: {
                if (arrayValue.size() != other.arrayValue.size()) return false;
                for (size_t i = 0; i < arrayValue.size(); ++i) {
                    if (!arrayValue[i].sameAs(other.arrayValue[i])) return false;
                }
                return true;
            }
            case Type::Object: {
                if (objectValue.size() != other.objectValue.size()) return false;
                for (const auto& kv : objectValue) {
                    const JsonValue* o = other.find(kv.first);
                    if (!o || !kv.second.sameAs(*o)) return false;
                }
                return true;
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Lenient JSON parser (BOM, comments, trailing commas) + serializer
// ---------------------------------------------------------------------------

struct JsonParseResult {
    JsonValue value;
    bool ok = false;
    std::string error;
};

namespace detail {

class JsonParser {
public:
    JsonParser(const char* begin, const char* end)
        : mBegin(begin), mCur(begin), mEnd(end) {}

    bool parse(JsonValue& out) {
        skipWs();
        if (!parseValue(out, 0)) return false;
        skipWs();
        if (mCur != mEnd) {
            return fail("trailing data after JSON document");
        }
        return true;
    }

private:
    const char* mBegin;
    const char* mCur;
    const char* mEnd;
    std::string mError;

    static constexpr int kMaxDepth = 64;

    bool fail(const char* what) {
        if (mError.empty()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s at byte %d", what,
                          static_cast<int>(mCur - mBegin));
            mError = buf;
        }
        return false;
    }

    void skipWs() {
        while (mCur != mEnd) {
            char c = *mCur;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++mCur;
            } else if (c == '/' && mCur + 1 != mEnd && mCur[1] == '/') {
                while (mCur != mEnd && *mCur != '\n') ++mCur;
            } else if (c == '/' && mCur + 1 != mEnd && mCur[1] == '*') {
                mCur += 2;
                while (mCur + 1 != mEnd && !(mCur[0] == '*' && mCur[1] == '/')) ++mCur;
                if (mCur + 1 != mEnd) mCur += 2;
                else mCur = mEnd;
            } else {
                break;
            }
        }
    }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return fail("JSON nesting too deep");
        skipWs();
        if (mCur == mEnd) return fail("unexpected end of input");
        char c = *mCur;
        switch (c) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string s;
                if (!parseString(s)) return false;
                out = JsonValue::makeString(std::move(s));
                return true;
            }
            case 't':
                return parseLiteral(out, "true", JsonValue::makeBool(true));
            case 'f':
                return parseLiteral(out, "false", JsonValue::makeBool(false));
            case 'n':
                return parseLiteral(out, "null", JsonValue{});
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
                return fail("unexpected character");
        }
    }

    bool parseLiteral(JsonValue& out, const char* literal, JsonValue value) {
        for (const char* p = literal; *p; ++p, ++mCur) {
            if (mCur == mEnd || *mCur != *p) return fail("invalid literal");
        }
        out = std::move(value);
        return true;
    }

    bool parseNumber(JsonValue& out) {
        const char* start = mCur;
        if (mCur != mEnd && *mCur == '-') ++mCur;
        while (mCur != mEnd && *mCur >= '0' && *mCur <= '9') ++mCur;
        if (mCur != mEnd && *mCur == '.') {
            ++mCur;
            while (mCur != mEnd && *mCur >= '0' && *mCur <= '9') ++mCur;
        }
        if (mCur != mEnd && (*mCur == 'e' || *mCur == 'E')) {
            ++mCur;
            if (mCur != mEnd && (*mCur == '+' || *mCur == '-')) ++mCur;
            while (mCur != mEnd && *mCur >= '0' && *mCur <= '9') ++mCur;
        }
        if (mCur == start) return fail("invalid number");
        std::string text(start, mCur);
        char* endPtr = nullptr;
        double v = std::strtod(text.c_str(), &endPtr);
        if (endPtr != text.c_str() + text.size()) return fail("invalid number");
        out = JsonValue::makeNumber(v);
        return true;
    }

    bool parseString(std::string& out) {
        if (mCur == mEnd || *mCur != '"') return fail("expected string");
        ++mCur;
        out.clear();
        while (mCur != mEnd) {
            unsigned char c = static_cast<unsigned char>(*mCur);
            if (c == '"') {
                ++mCur;
                return true;
            }
            if (c == '\\') {
                ++mCur;
                if (mCur == mEnd) return fail("unterminated escape");
                char e = *mCur++;
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!parseHex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            mCur + 1 != mEnd && mCur[0] == '\\' && mCur[1] == 'u') {
                            mCur += 2;
                            unsigned low = 0;
                            if (!parseHex4(low)) return false;
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                encodeUtf8(out, cp);
                                cp = low; // unpaired high surrogate: emit as-is
                            }
                        }
                        encodeUtf8(out, cp);
                        break;
                    }
                    default:
                        return fail("invalid escape");
                }
            } else {
                out.push_back(static_cast<char>(c));
                ++mCur;
            }
        }
        return fail("unterminated string");
    }

    bool parseHex4(unsigned& out) {
        if (mEnd - mCur < 4) return fail("truncated \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *mCur++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        out = v;
        return true;
    }

    static void encodeUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseObject(JsonValue& out, int depth) {
        ++mCur; // '{'
        JsonValue obj = JsonValue::makeObject();
        skipWs();
        if (mCur != mEnd && *mCur == '}') {
            ++mCur;
            out = std::move(obj);
            return true;
        }
        while (true) {
            skipWs();
            std::string key;
            if (mCur == mEnd || *mCur != '"') return fail("expected object key");
            if (!parseString(key)) return false;
            skipWs();
            if (mCur == mEnd || *mCur != ':') return fail("expected ':'");
            ++mCur;
            JsonValue value;
            if (!parseValue(value, depth + 1)) return false;
            if (obj.find(key) == nullptr) {
                obj.objectValue.emplace_back(std::move(key), std::move(value));
            } else {
                obj.set(key, std::move(value)); // duplicate key: last wins
            }
            skipWs();
            if (mCur == mEnd) return fail("unterminated object");
            if (*mCur == ',') {
                ++mCur;
                skipWs();
                if (mCur != mEnd && *mCur == '}') { // trailing comma
                    ++mCur;
                    out = std::move(obj);
                    return true;
                }
                continue;
            }
            if (*mCur == '}') {
                ++mCur;
                out = std::move(obj);
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(JsonValue& out, int depth) {
        ++mCur; // '['
        JsonValue arr = JsonValue::makeArray();
        skipWs();
        if (mCur != mEnd && *mCur == ']') {
            ++mCur;
            out = std::move(arr);
            return true;
        }
        while (true) {
            JsonValue value;
            if (!parseValue(value, depth + 1)) return false;
            arr.arrayValue.push_back(std::move(value));
            skipWs();
            if (mCur == mEnd) return fail("unterminated array");
            if (*mCur == ',') {
                ++mCur;
                skipWs();
                if (mCur != mEnd && *mCur == ']') { // trailing comma
                    ++mCur;
                    out = std::move(arr);
                    return true;
                }
                continue;
            }
            if (*mCur == ']') {
                ++mCur;
                out = std::move(arr);
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }
};

} // namespace detail

inline JsonParseResult parseJsonLenient(const std::string& text) {
    JsonParseResult result;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    // UTF-8 / UTF-16LE BOM
    if (end - begin >= 3 && static_cast<unsigned char>(begin[0]) == 0xEF &&
        static_cast<unsigned char>(begin[1]) == 0xBB &&
        static_cast<unsigned char>(begin[2]) == 0xBF) {
        begin += 3;
    } else if (end - begin >= 2 && static_cast<unsigned char>(begin[0]) == 0xFF &&
               static_cast<unsigned char>(begin[1]) == 0xFE) {
        result.error = "UTF-16 input is not supported";
        return result;
    }
    detail::JsonParser parser(begin, end);
    if (parser.parse(result.value)) {
        result.ok = true;
    }
    return result;
}

// Numbers print without a forced ".0" so `"format_version": 2` stays an
// integer and `"scale": 1.5` keeps its fraction. Non-finite values (not
// representable in JSON) become 0.
inline void dumpNumber(std::ostringstream& out, double v) {
    if (!std::isfinite(v)) v = 0.0;
    double rounded = v < 0 ? -v : v;
    rounded = (rounded - static_cast<long long>(rounded));
    if (rounded == 0.0 && v > -9.0e15 && v < 9.0e15) {
        out << static_cast<long long>(v);
    } else {
        out << v;
    }
}

inline void dumpJsonTo(std::ostringstream& out, const JsonValue& value, int indent, int depth) {
    auto writeIndent = [&](int level) {
        out << '\n';
        for (int i = 0; i < level * 2; ++i) out << ' ';
    };
    auto writeEscaped = [&](const std::string& s) {
        out << '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out << buf;
                    } else {
                        out << static_cast<char>(c);
                    }
            }
        }
        out << '"';
    };

    if (depth > 64) { out << "null"; return; }
    switch (value.type) {
        case JsonValue::Type::Null: out << "null"; break;
        case JsonValue::Type::Bool: out << (value.boolValue ? "true" : "false"); break;
        case JsonValue::Type::Number: dumpNumber(out, value.numberValue); break;
        case JsonValue::Type::String: writeEscaped(value.stringValue); break;
        case JsonValue::Type::Array: {
            if (value.arrayValue.empty()) { out << "[]"; break; }
            out << '[';
            for (size_t i = 0; i < value.arrayValue.size(); ++i) {
                if (i) out << ',';
                writeIndent(depth + 1);
                dumpJsonTo(out, value.arrayValue[i], indent, depth + 1);
            }
            writeIndent(depth);
            out << ']';
            break;
        }
        case JsonValue::Type::Object: {
            if (value.objectValue.empty()) { out << "{}"; break; }
            out << '{';
            bool first = true;
            for (const auto& kv : value.objectValue) {
                if (!first) out << ',';
                first = false;
                writeIndent(depth + 1);
                writeEscaped(kv.first);
                out << ": ";
                dumpJsonTo(out, kv.second, indent, depth + 1);
            }
            writeIndent(depth);
            out << '}';
            break;
        }
    }
}

inline std::string dumpJson(const JsonValue& value) {
    std::ostringstream out;
    dumpJsonTo(out, value, 2, 0);
    return out.str();
}

// ---------------------------------------------------------------------------
// Deep merge
// ---------------------------------------------------------------------------

// Array keys whose entries from different packs should be combined instead of
// overridden. `scripts.animate` binds named animations to an entity; both
// packs' bindings must survive the merge or one pack's content goes inert.
inline bool isUnionArrayKey(const std::string& key) {
    return key == "animate";
}

// `top` wins every conflict; keys (recursively: whole subtrees) that only
// exist in `lower` are copied into `top`. Union keys append the lower pack's
// array entries that the top does not already have, after the top's own.
inline void mergeJsonInto(JsonValue& top, const JsonValue& lower) {
    if (!top.isObject() || !lower.isObject()) return;
    for (const auto& kv : lower.objectValue) {
        JsonValue* existing = top.find(kv.first);
        if (!existing) {
            top.objectValue.emplace_back(kv.first, kv.second);
            continue;
        }
        if (existing->isObject() && kv.second.isObject()) {
            mergeJsonInto(*existing, kv.second);
        } else if (existing->isArray() && kv.second.isArray() && isUnionArrayKey(kv.first)) {
            for (const JsonValue& entry : kv.second.arrayValue) {
                bool present = false;
                for (const JsonValue& e : existing->arrayValue) {
                    if (e.sameAs(entry)) { present = true; break; }
                }
                if (!present) existing->arrayValue.push_back(entry);
            }
        }
        // otherwise: top wins, keep as-is
    }
}

// ---------------------------------------------------------------------------
// .lang merge (key=value lines; top file wins, unique lower keys appended)
// ---------------------------------------------------------------------------

struct LangMergeResult {
    std::string text;
    int addedKeys = 0;
};

inline void extractLangKeys(const std::string& text, std::map<std::string, bool>& keys) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string key = line.substr(0, eq);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            if (!key.empty()) keys[key] = true;
        }
        if (end == text.size()) break;
        start = end + 1;
    }
}

inline LangMergeResult mergeLangText(const std::string& topText, const std::string& lowerText) {
    LangMergeResult result;
    std::map<std::string, bool> topKeys;
    extractLangKeys(topText, topKeys);

    result.text = topText;
    if (!result.text.empty() && result.text.back() != '\n') result.text += '\n';

    std::string additions;
    size_t start = 0;
    while (start <= lowerText.size()) {
        size_t end = lowerText.find('\n', start);
        if (end == std::string::npos) end = lowerText.size();
        std::string line = lowerText.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r')) line.pop_back();

        size_t eq = line.find('=');
        bool isKeyLine = eq != std::string::npos && eq > 0;
        if (isKeyLine) {
            std::string key = line.substr(0, eq);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            if (!topKeys[key]) {
                topKeys[key] = true;
                additions += line;
                additions += '\n';
                ++result.addedKeys;
            }
        }
        if (end == lowerText.size()) break;
        start = end + 1;
    }

    if (result.addedKeys > 0) {
        result.text += "# keys added by BedrockToolsPlus Pack Merger\n";
        result.text += additions;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Path helpers (Bedrock resource paths are lowercase, forward-slash)
// ---------------------------------------------------------------------------

inline std::string normalizeRelPath(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        char n = (c == '\\') ? '/' : c;
        n = static_cast<char>(std::tolower(static_cast<unsigned char>(n)));
        if (n == '/' && !out.empty() && out.back() == '/') continue;
        out.push_back(n);
    }
    // strip leading "./" or "/"
    size_t start = 0;
    while (start < out.size()) {
        if (out[start] == '/') { ++start; continue; }
        if (out[start] == '.' && start + 1 < out.size() && out[start + 1] == '/') { start += 2; continue; }
        break;
    }
    return out.substr(start);
}

inline bool isJsonRelPath(const std::string& rel) {
    return rel.size() >= 5 && rel.compare(rel.size() - 5, 5, ".json") == 0;
}

inline bool isLangRelPath(const std::string& rel) {
    return rel.size() >= 5 && rel.compare(rel.size() - 5, 5, ".lang") == 0;
}

// Paths that must never be merged or copied into the generated pack:
// manifests define identity (the generated pack has its own), pack icons are
// cosmetic, and `subpacks/` are alternate quality tiers with their own
// internal override semantics that the merge engine intentionally ignores.
inline bool isSkippedRelPath(const std::string& rel) {
    if (rel == "manifest.json" || rel == "pack_icon.png") return true;
    if (rel.rfind("subpacks/", 0) == 0) return true;
    if (rel == ".ds_store" || rel == "desktop.ini") return true;
    return false;
}

// ---------------------------------------------------------------------------
// Merge plan (filesystem walk + conflict resolution)
// ---------------------------------------------------------------------------

// One participating pack. `packs` is ordered LOWEST priority first, i.e. the
// same order as `global_resource_packs.json` / `world_resource_packs.json`
// (the last entry is applied last and overrides the ones below it).
struct PackFolder {
    std::string name;   // display name from the manifest (or folder name)
    std::string root;   // absolute folder path of the pack
    std::string uuid;   // header uuid (lowercase), used to exclude the pack itself
};

struct MergedFile {
    std::string relPath;     // normalized lowercase path, e.g. "entity/player.entity.json"
    std::string content;     // merged text to write into the generated pack
    int topPack = -1;        // index (into the input vector) of the winning pack
    int mergedFrom = 0;      // how many packs contained this path
    bool copyOfTop = false;  // true when a file failed to parse and the top
                             // pack's raw bytes were kept instead (vanilla
                             // behavior for that file)
};

struct MergePlan {
    std::vector<MergedFile> files;
    std::vector<std::string> packNames; // participating packs, lowest first
    std::string note;                   // human-readable status line
};

namespace detail {

inline bool readFileIfAvailable(const std::filesystem::path& path, std::string& out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

inline void collectPackFiles(const std::string& root, std::map<std::string, std::string>& out) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    while (it != end) {
        std::error_code entryEc;
        const std::filesystem::file_status status = it->status(entryEc);
        if (!entryEc && std::filesystem::is_regular_file(status)) {
            std::string rel = normalizeRelPath(std::filesystem::relative(it->path(), root, entryEc).generic_string());
            if (!entryEc && !rel.empty() && !isSkippedRelPath(rel)) {
                out.emplace(rel, it->path().generic_string()); // first occurrence wins
            }
        }
        it.increment(ec);
        if (ec) break;
    }
}

} // namespace detail

inline MergePlan buildMergePlan(const std::vector<PackFolder>& packsLowestFirst) {
    MergePlan plan;
    for (const PackFolder& pack : packsLowestFirst) {
        plan.packNames.push_back(pack.name);
    }
    if (packsLowestFirst.size() < 2) {
        plan.note = "needs at least 2 resource packs";
        return plan;
    }

    // rel path -> packs containing it (indices ascending in priority)
    std::map<std::string, std::vector<int>> occurrences;
    std::vector<std::map<std::string, std::string>> packFiles(packsLowestFirst.size());
    for (size_t i = 0; i < packsLowestFirst.size(); ++i) {
        detail::collectPackFiles(packsLowestFirst[i].root, packFiles[i]);
        for (const auto& kv : packFiles[i]) {
            occurrences[kv.first].push_back(static_cast<int>(i));
        }
    }

    int jsonMerged = 0, langMerged = 0, fallbacks = 0;

    for (const auto& kv : occurrences) {
        const std::string& rel = kv.first;
        const std::vector<int>& owners = kv.second;
        if (owners.size() < 2) continue;

        const bool jsonFile = isJsonRelPath(rel);
        const bool langFile = isLangRelPath(rel);
        if (!jsonFile && !langFile) continue; // binary: top pack already wins

        const int topIdx = owners.back();
        MergedFile file;
        file.relPath = rel;
        file.topPack = topIdx;
        file.mergedFrom = static_cast<int>(owners.size());

        // Every owner's bytes, lowest priority first.
        std::vector<std::string> texts;
        texts.reserve(owners.size());
        bool readable = true;
        for (int idx : owners) {
            std::string text;
            if (!detail::readFileIfAvailable(packFiles[idx][rel], text)) {
                readable = false;
                break;
            }
            texts.push_back(std::move(text));
        }
        if (!readable || texts.empty()) continue;

        if (langFile) {
            // Final = top pack's file plus unique keys of every lower pack.
            std::string acc = texts.back();
            for (size_t i = 0; i + 1 < texts.size(); ++i) {
                LangMergeResult step = mergeLangText(acc, texts[i]);
                acc = std::move(step.text);
            }
            file.content = acc;
            file.copyOfTop = false;
            ++langMerged;
        } else {
            std::vector<JsonValue> docs(texts.size());
            bool allParsed = true;
            for (size_t i = 0; i < texts.size(); ++i) {
                JsonParseResult parsed = parseJsonLenient(texts[i]);
                if (!parsed.ok) { allParsed = false; break; }
                docs[i] = std::move(parsed.value);
            }
            if (!allParsed) {
                // One of the packs ships invalid JSON: keep the top pack's
                // bytes so this file behaves exactly like the unmerged game.
                file.content = texts.back();
                file.copyOfTop = true;
                ++fallbacks;
            } else {
                JsonValue merged = docs.front();
                for (size_t i = 1; i < docs.size(); ++i) {
                    // Higher pack wins: copy its document, then fill the keys
                    // it lacks from the accumulated lower-priority merge.
                    JsonValue higher = docs[i];
                    mergeJsonInto(higher, merged);
                    merged = std::move(higher);
                }
                file.content = dumpJson(merged);
                file.copyOfTop = false;
                ++jsonMerged;
            }
        }

        plan.files.push_back(std::move(file));
    }

    std::ostringstream note;
    note << plan.packNames.size() << " packs, " << plan.files.size()
         << " shared file(s): " << jsonMerged << " json merged";
    if (langMerged) note << ", " << langMerged << " lang merged";
    if (fallbacks) note << ", " << fallbacks << " kept from top pack (unparseable)";
    plan.note = note.str();
    return plan;
}

} // namespace bedrocktools::modules::packmerger
