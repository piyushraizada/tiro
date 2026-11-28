// punctuation.cpp — Metaspace-BPE tokenizer + ModernBERT punctuation model on the
// ONNX Runtime that is already statically linked into Tiro for ASR.
//
// Algorithm is a one-to-one port of the model's own reference punctuate.py plus the
// HuggingFace fast tokenizer's Metaspace/BPE pipeline:
//   normalize: " " -> U+2581 ; prepend U+2581 ; every U+2581 starts a piece
//   per piece: split into codepoints -> vocab ids (missing -> <0xNN> byte fallback
//   over the UTF-8 bytes -> unk), then greedy pair-rank BPE merges (no ## markers;
//   word starts carry the U+2581 inside the token string)
//   window 400 subtokens with 50 overlap, input [BOS]+ids+[EOS], argmax over the
//   6 classes, emit a mark at a token end only where the next char is a space/end
// Verified byte-exact against HuggingFace token ids + offset mappings; the
// --polish-tokens switch below replays that comparison without a mic.

#include "punctuation.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "onnxruntime_c_api.h"

namespace punct {

// ---------------------------------------------------------------- module state

static void (*g_log)(const wchar_t*) = nullptr;
static void Log(const wchar_t* m) { if (g_log) g_log(m); }

static bool g_ready = false;

// vocab: token (UTF-8) <-> id
static std::unordered_map<std::string, int> g_vocabId;
static std::vector<std::string> g_vocabTok;          // id -> token
// merge pair -> rank (first occurrence wins, same as HF)
static std::unordered_map<uint64_t, int> g_mergeRank;
static int g_bos = 2, g_unk = 3;
static std::map<int, wchar_t> g_id2mark;            // class id -> punctuation char
static int g_classes = 0;

static const OrtApi* g_api = nullptr;
static OrtEnv* g_env = nullptr;
static OrtSession* g_sess = nullptr;
static OrtMemoryInfo* g_meminfo = nullptr;

static const char* g_inNames[2] = { "input_ids", "attention_mask" };
static const char* g_outNames[1] = { "logits" };

static const wchar_t kMeta = 0x2581;                 // U+2581 LOWER ONE EIGHTH BLOCK
static const size_t kChunkTokens = 400, kOverlapTokens = 50;

static inline uint64_t pairKey(int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; }

// ---------------------------------------------------------------- small helpers

static bool ReadAll(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0) { CloseHandle(h); return false; }
    out.resize((size_t)li.QuadPart);
    size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        DWORD want = (DWORD)std::min<size_t>(out.size() - done, 1u << 20);
        if (!ReadFile(h, &out[done], want, &got, nullptr) || got == 0) {
            CloseHandle(h); out.clear(); return false;
        }
        done += got;
    }
    CloseHandle(h);
    return true;
}

static std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// JSON-unescape s[1..close) (the span between the quotes) into UTF-8.
static std::string Unescape(const char* p, size_t n)
{
    std::string out; out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        char c = p[i];
        if (c != '\\') { out.push_back(c); continue; }
        if (++i >= n) break;
        switch (p[i]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 >= n) { i = n; break; }
                unsigned v = (unsigned)strtoul(std::string(p + i + 1, 4).c_str(), nullptr, 16);
                i += 4;
                if (v >= 0xD800 && v < 0xDC00 && i + 6 < n && p[i + 1] == '\\' && p[i + 2] == 'u') {
                    unsigned lo = (unsigned)strtoul(std::string(p + i + 3, 4).c_str(), nullptr, 16);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                // encode v as UTF-8
                if (v < 0x80) out.push_back((char)v);
                else if (v < 0x800) { out.push_back((char)(0xC0 | (v >> 6))); out.push_back((char)(0x80 | (v & 0x3F))); }
                else if (v < 0x10000) { out.push_back((char)(0xE0 | (v >> 12))); out.push_back((char)(0x80 | ((v >> 6) & 0x3F))); out.push_back((char)(0x80 | (v & 0x3F))); }
                else { out.push_back((char)(0xF0 | (v >> 18))); out.push_back((char)(0x80 | ((v >> 12) & 0x3F))); out.push_back((char)(0x80 | ((v >> 6) & 0x3F))); out.push_back((char)(0x80 | (v & 0x3F))); }
                break;
            }
            default: out.push_back(p[i]); break;
        }
    }
    return out;
}

// Find the char just past the colon of key `key` ("key":) at or after pos.
static const char* FindKey(const char* pos, const char* key)
{
    std::string pat = std::string("\"") + key + "\"";
    const char* f = std::strstr(pos, pat.c_str());
    if (!f) return nullptr;
    f += pat.size();
    while (*f == ' ' || *f == '\t' || *f == '\n' || *f == '\r') ++f;
    if (*f != ':') return nullptr;
    ++f;
    while (*f == ' ' || *f == '\t' || *f == '\n' || *f == '\r') ++f;
    return f;
}

// Skip one JSON string starting at `p` (which must be a '"'); return char after close.
static const char* SkipString(const char* p, const char* end)
{
    ++p;
    while (p < end) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"') return p + 1;
        ++p;
    }
    return nullptr;
}

static const char* ParseInt(const char* p, long long& v)
{
    char* e = nullptr;
    v = strtoll(p, &e, 10);
    return e;
}

// ---------------------------------------------------------------- tokenizer.json load

static bool LoadTokenizer(const std::wstring& path)
{
    std::string js;
    if (!ReadAll(path, js)) return false;
    const char* begin = js.data();
    const char* end = js.data() + js.size();

    // vocab
    const char* p = FindKey(begin, "vocab");
    if (!p || *p != '{') return false;
    ++p;
    int expect = 0;
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r' || *p == ',')) ++p;
        if (p >= end) return false;
        if (*p == '}') { ++p; break; }
        if (*p != '"') return false;
        const char* close = SkipString(p, end);
        if (!close) return false;
        std::string tok = Unescape(p + 1, (size_t)(close - p - 2));
        p = close;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p != ':') return false;
        ++p;
        long long id = 0;
        p = ParseInt(p, id);
        // A token id is data from the file. A value in the billions would ask for a
        // resize no machine can serve, and id + 1 would wrap - refuse instead of guessing.
        if (id < 0 || id > 1000000) return false;
        if (g_vocabId.count(tok)) return false;      // duplicate token: refuse, don't guess
        g_vocabId.emplace(tok, (int)id);
        if ((int)g_vocabTok.size() < id + 1) g_vocabTok.resize(id + 1);
        g_vocabTok[(size_t)id] = std::move(tok);
        ++expect;
    }

    // merges: array of ["a","b"] pairs (older format: "a b" strings); rank = index
    p = FindKey(p, "merges");
    if (!p || *p != '[') return false;
    ++p;
    int rank = 0;
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r' || *p == ',')) ++p;
        if (p >= end) return false;
        if (*p == ']') { ++p; break; }
        std::string a, b;
        if (*p == '[') {
            ++p;
            while (*p != '"') ++p;
            const char* close = SkipString(p, end);
            if (!close) return false;
            a = Unescape(p + 1, (size_t)(close - p - 2));
            p = close;
            while (*p != '"') ++p;
            close = SkipString(p, end);
            if (!close) return false;
            b = Unescape(p + 1, (size_t)(close - p - 2));
            p = close;
            while (*p != ']') ++p;
            ++p;
        } else if (*p == '"') {
            const char* close = SkipString(p, end);
            if (!close) return false;
            std::string ab = Unescape(p + 1, (size_t)(close - p - 2));
            p = close;
            size_t sp = ab.find(' ');
            if (sp == std::string::npos) return false;
            a = ab.substr(0, sp); b = ab.substr(sp + 1);
        } else return false;
        auto ia = g_vocabId.find(a), ib = g_vocabId.find(b);
        if (ia != g_vocabId.end() && ib != g_vocabId.end()) {
            uint64_t k = pairKey(ia->second, ib->second);
            if (!g_mergeRank.count(k)) g_mergeRank.emplace(k, rank);   // first rank wins, as HF
        }
        ++rank;
    }

    auto bos = g_vocabId.find("<bos>"); if (bos != g_vocabId.end()) g_bos = bos->second;
    auto unk = g_vocabId.find("<unk>"); if (unk != g_vocabId.end()) g_unk = unk->second;
    return !g_vocabId.empty();
}

static bool LoadConfig(const std::wstring& path)
{
    std::string js;
    if (!ReadAll(path, js)) return false;
    const char* p = FindKey(js.data(), "id2label");
    if (!p || *p != '{') return false;
    ++p;
    const char* end = js.data() + js.size();
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r' || *p == ',')) ++p;
        if (p >= end) return false;
        if (*p == '}') break;
        if (*p != '"') return false;
        const char* close = SkipString(p, end);
        if (!close) return false;
        std::string key = Unescape(p + 1, (size_t)(close - p - 2));
        p = close;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p != ':') return false;
        ++p;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p != '"') return false;
        close = SkipString(p, end);
        if (!close) return false;
        std::string lab = Unescape(p + 1, (size_t)(close - p - 2));
        p = close;
        int id = atoi(key.c_str());
        ++g_classes;
        if (lab.size() > 1) {
            // multi-char label: 'O'/'0'/pad-like -> no mark
            continue;
        }
        if (lab.empty() || lab == "0" || lab == "O") continue;
        // label is UTF-8; decode to one wchar
        std::wstring w;
        int n = MultiByteToWideChar(CP_UTF8, 0, lab.data(), (int)lab.size(), nullptr, 0);
        w.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, lab.data(), (int)lab.size(), &w[0], n);
        if (!w.empty() && w[0] != L'0') {
            // full-width -> ASCII, so output never carries wide punctuation into the target
            switch (w[0]) {
                case 0x3002: w[0] = L'.'; break;   // 。
                case 0xFF0C: w[0] = L','; break;   // ，
                case 0xFF1A: w[0] = L':'; break;   // ：
                case 0xFF1F: w[0] = L'?'; break;   // ？
                case 0xFF01: w[0] = L'!'; break;   // ！
                default: break;
            }
            g_id2mark[id] = w[0];
        }
    }
    return g_classes > 0;
}

// ---------------------------------------------------------------- tokenizer

// Split a wstring into codepoint groups (surrogate pairs kept whole).
static void Codepoints(const std::wstring& s, size_t from, size_t to, std::vector<std::wstring>& out)
{
    for (size_t i = from; i < to; ) {
        wchar_t c = s[i];
        size_t len = 1;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < to && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
            len = 2;
        out.push_back(s.substr(i, len));
        i += len;
    }
}

static std::string ByteTok(int b)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "<0x%02X>", b);
    return std::string(buf);
}

struct Encoded {
    std::vector<int64_t> ids;
    std::vector<std::pair<int, int>> spans;          // [start,end) in the input wstring
};

// One word piece (T[s,e), T[s]==kMeta) -> subtoken ids + wchar widths.
// Plain Sennrich BPE over codepoints (this is what the tokenizers BPE does with a
// Metaspace pre-tokenizer): initial sequence = one token per codepoint (missing ->
// UTF-8 <0xNN> byte fallback -> <unk>), then repeatedly apply the lowest-rank pair
// from merges whose joined string is in vocab. Widths track original-string chars.
static void BpePiece(const std::wstring& T, size_t s, size_t e,
                     std::vector<int64_t>& ids, std::vector<int>& widths)
{
    std::vector<std::wstring> cps;
    // The stream's very first piece carries a synthetic prepended marker (T[0]) that has
    // no counterpart in the original text: drop it from the sequence and start its width
    // at 0. Every other piece's kMeta is a real space and counts from the start.
    size_t cs = (s == 0) ? 1 : s;
    Codepoints(T, cs, e, cps);

    std::vector<int> tid;
    std::vector<std::string> tstr;
    std::vector<int> tw;
    tid.reserve(cps.size()); tstr.reserve(cps.size()); tw.reserve(cps.size());
    if (s == 0) { tid.push_back(g_vocabId["\xE2\x96\x81"]); tstr.push_back("\xE2\x96\x81"); tw.push_back(0); }

    for (auto& cp : cps) {
        std::string u8 = Utf8(cp);
        auto it = g_vocabId.find(u8);
        if (it != g_vocabId.end()) {
            tid.push_back(it->second); tstr.push_back(std::move(u8)); tw.push_back((int)cp.size());
        } else {
            std::vector<std::string> bytes;
            for (unsigned char b : u8) bytes.push_back(ByteTok(b));
            bool all = true;
            for (auto& bt : bytes) if (!g_vocabId.count(bt)) { all = false; break; }
            if (all) {
                for (size_t k = 0; k < bytes.size(); ++k) {
                    tid.push_back(g_vocabId[bytes[k]]);
                    tw.push_back(k == 0 ? (int)cp.size() : 0);
                    tstr.push_back(std::move(bytes[k]));
                }
            } else {
                tid.push_back(g_unk); tstr.push_back("<unk>"); tw.push_back((int)cp.size());
            }
        }
    }

    std::unordered_set<uint64_t> blocked;
    while (tid.size() > 1) {
        int best = -1; int bestRank = INT_MAX;
        for (size_t i = 0; i + 1 < tid.size(); ++i) {
            uint64_t k = pairKey(tid[i], tid[i + 1]);
            if (blocked.count(k)) continue;
            auto m = g_mergeRank.find(k);
            if (m != g_mergeRank.end() && m->second < bestRank) { bestRank = m->second; best = (int)i; }
        }
        if (best < 0) break;
        std::string merged = tstr[best] + tstr[best + 1];
        auto it = g_vocabId.find(merged);
        if (it == g_vocabId.end()) { blocked.insert(pairKey(tid[best], tid[best + 1])); continue; }
        tw[best] += tw[best + 1];    // the joined token spans both parts
        tid.erase(tid.begin() + best + 1);
        tstr.erase(tstr.begin() + best + 1);
        tw.erase(tw.begin() + best + 1);
        tstr[best] = std::move(merged);
        tid[best] = it->second;      // tw[best] already spans both parts
    }

    for (size_t i = 0; i < tid.size(); ++i) {
        ids.push_back(tid[i]);
        widths.push_back(tw[i]);
    }
}

// Full encode of the (already preprocessed) text: pieces + BPE, with offsets.
// T = kMeta + text-with-spaces-as-Meta. A piece T[k..m) maps to original
// [k==0 ? 0 : k-1, m-1); its subtokens split that span left to right.
static void Encode(const std::wstring& text, Encoded& out)
{
    std::wstring T; T.reserve(text.size() + 1);
    T.push_back(kMeta);
    for (wchar_t c : text) T.push_back(c == L' ' ? kMeta : c);

    std::vector<int> widths;
    for (size_t k = 0; k < T.size(); ) {
        size_t m = k + 1;
        while (m < T.size() && T[m] != kMeta) ++m;
        int os = (k == 0) ? 0 : (int)k - 1;
        size_t before = out.ids.size();
        BpePiece(T, k, m, out.ids, widths);
        size_t after = out.ids.size();
        int pos = os;
        for (size_t t = before; t < after; ++t) {
            int w = widths[t];
            out.spans.emplace_back(pos, pos + w);
            pos += w;
        }
        k = m;
    }
}

// ---------------------------------------------------------------- ORT

static bool InitOrt(const std::wstring& modelPath, int threads)
{
    const OrtApiBase* base = OrtGetApiBase();
    // The vendored runtime may predate this header's ORT_API_VERSION; GetApi()
    // answers for any version the lib implements, and every call used here
    // (session create/run/tensor) has existed since the API's first revision.
    for (unsigned v = ORT_API_VERSION; v >= 1 && !g_api; --v) g_api = base->GetApi(v);
    if (!g_api) { Log(L"polish: onnxruntime api unavailable"); return false; }

    if (g_api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "tiro-punct", &g_env) != nullptr) return false;
    OrtSessionOptions* opts = nullptr;
    if (g_api->CreateSessionOptions(&opts) != nullptr) return false;
    g_api->SetIntraOpNumThreads(opts, threads);
    g_api->SetSessionExecutionMode(opts, ORT_SEQUENTIAL);

    if (g_api->CreateSession(g_env, modelPath.c_str(), opts, &g_sess) != nullptr) {
        Log(L"polish: session creation failed");
        return false;
    }
    g_api->ReleaseSessionOptions(opts);

    size_t nIn = 0, nOut = 0;
    if (g_api->SessionGetInputCount(g_sess, &nIn) != nullptr || nIn != 2 ||
        g_api->SessionGetOutputCount(g_sess, &nOut) != nullptr || nOut != 1) {
        Log(L"polish: unexpected model input/output count");
        return false;
    }
    if (g_api->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &g_meminfo) != nullptr) return false;
    return true;
}

// Run one window: ids[a,b) -> class id per token, written into cls[].
static bool RunWindow(const std::vector<int64_t>& ids, size_t a, size_t b, std::vector<int>& cls)
{
    const int64_t L = (int64_t)(b - a) + 2;
    std::vector<int64_t> x((size_t)L), att((size_t)L, 1);
    x[0] = g_bos;
    for (size_t k = a; k < b; ++k) x[k - a + 1] = ids[k];
    x[(size_t)L - 1] = 1;                             // eos

    const int64_t dims[2] = { 1, L };
    OrtValue* tv[2] = { nullptr, nullptr };
    if (g_api->CreateTensorWithDataAsOrtValue(g_meminfo, x.data(), x.size() * sizeof(int64_t),
                                              dims, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &tv[0]) != nullptr)
        return false;
    if (g_api->CreateTensorWithDataAsOrtValue(g_meminfo, att.data(), att.size() * sizeof(int64_t),
                                              dims, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &tv[1]) != nullptr) {
        g_api->ReleaseValue(tv[0]);
        return false;
    }
    OrtValue* out = nullptr;
    OrtStatus* st = g_api->Run(g_sess, nullptr, g_inNames, tv, 2, g_outNames, 1, &out);
    g_api->ReleaseValue(tv[0]);
    g_api->ReleaseValue(tv[1]);
    if (st != nullptr) { g_api->ReleaseStatus(st); return false; }

    OrtTensorTypeAndShapeInfo* shape = nullptr;
    bool ok = g_api->GetTensorTypeAndShape(out, &shape) == nullptr;
    size_t rank = 0;
    if (ok) ok = g_api->GetDimensionsCount(shape, &rank) == nullptr && rank == 3;
    int64_t d3[3] = { 0, 0, 0 };
    if (ok) ok = g_api->GetDimensions(shape, d3, 3) == nullptr;
    float* data = nullptr;
    if (ok) ok = g_api->GetTensorMutableData(out, (void**)&data) == nullptr;
    if (ok && d3[2] == g_classes && d3[1] == L) {
        const int C = (int)d3[2];
        int hist[8] = { 0 };
        for (size_t k = a; k < b; ++k) {
            const float* row = data + ((int64_t)(k - a) + 1) * C;
            int best = 0; float bv = row[0];
            for (int c = 1; c < C; ++c) if (row[c] > bv) { bv = row[c]; best = c; }
            cls[k] = best;
            if (best < 8) hist[best]++;
        }
        if (g_log) {
            wchar_t b2[160];
            swprintf_s(b2, L"polish dbg window [%zu,%zu): hist %d,%d,%d,%d,%d,%d", a, b,
                       hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
            g_log(b2);
        }
    } else {
        ok = false;
    }
    if (shape) g_api->ReleaseTensorTypeAndShapeInfo(shape);
    g_api->ReleaseValue(out);
    return ok;
}

// ---------------------------------------------------------------- public API

bool Init(const std::wstring& modelDir, void (*logFn)(const wchar_t*), int threads)
{
    g_log = logFn;
    if (g_ready) return true;

    ULONGLONG t0 = GetTickCount64();
    if (!LoadTokenizer(modelDir + L"\\tokenizer.json")) { Log(L"polish: tokenizer.json load failed"); return false; }
    ULONGLONG t1 = GetTickCount64();
    if (!LoadConfig(modelDir + L"\\config.json")) { Log(L"polish: config.json load failed"); return false; }
    wchar_t b[160];
    swprintf_s(b, L"polish: tokenizer ready in %llu ms (%zu tokens, %zu merges)",
               (unsigned long long)(t1 - t0), g_vocabId.size(), g_mergeRank.size());
    Log(b);

    if (!InitOrt(modelDir + L"\\model_quantized.onnx", threads ? threads : 1)) return false;
    swprintf_s(b, L"polish: model ready in %llu ms", (unsigned long long)(GetTickCount64() - t0));
    Log(b);
    g_ready = true;
    return true;
}

bool Ready() { return g_ready; }

// ---------------------------------------------------------------- polish pass

// Marks the model may emit plus common typographic ones, for stripping input marks.
static bool IsMark(wchar_t c)
{
    static const wchar_t* m = L".,;:!?\uFF0C\u3002\uFF1A\uFF1F\uFF01\uFF1B\u2026\u0964\u0965\u06D4\u1662\u1663\u1667";
    return wcschr(m, c) != nullptr;
}
static bool IsDigitC(wchar_t c) { return c >= L'0' && c <= L'9'; }

// Preprocess: remove existing marks (keep decimal/colon-embedded ones between digits),
// collapse whitespace runs to single spaces, trim. Everything else untouched.
static std::wstring Preprocess(const std::wstring& raw)
{
    std::wstring s;
    s.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        wchar_t c = raw[i];
        if (IsMark(c)) {
            bool keep = (c == L'.' || c == L',' || c == L':') &&
                        i > 0 && i + 1 < raw.size() && IsDigitC(raw[i - 1]) && IsDigitC(raw[i + 1]);
            if (keep) s.push_back(c);
            continue;
        }
        s.push_back(c);
    }
    std::wstring o;
    o.reserve(s.size());
    bool inSp = false;
    for (wchar_t c : s) {
        if (iswspace(c)) { inSp = true; continue; }
        if (inSp && !o.empty()) o.push_back(L' ');
        inSp = false;
        o.push_back(c);
    }
    return o;
}

static std::wstring Capitalize(const std::wstring& s)
{
    std::wstring o = s;
    auto upperAt = [&](size_t i) { if (i < o.size() && o[i] >= L'a' && o[i] <= L'z') o[i] -= 32; };
    upperAt(0);
    for (size_t i = 0; i + 1 < o.size(); ++i) {
        if (o[i] == L'.' || o[i] == L'!' || o[i] == L'?') {
            size_t j = i + 1;
            while (j < o.size() && iswspace(o[j])) ++j;
            upperAt(j);
        }
    }
    // pronoun I
    for (size_t i = 0; i < o.size(); ++i) {
        if (o[i] != L'i') continue;
        bool leftOk = (i == 0) || !(iswalnum(o[i - 1]) || o[i - 1] == L'\'');
        bool rightOk = (i + 1 >= o.size()) || !(iswalnum(o[i + 1]) || o[i + 1] == L'\'');
        if (leftOk && rightOk) o[i] = L'I';
    }
    return o;
}

std::wstring Polish(const std::wstring& text)
{
    if (!g_ready) return {};
    std::wstring proc = Preprocess(text);
    if (proc.empty()) return {};

    Encoded enc;
    Encode(proc, enc);
    if (enc.ids.empty()) return {};

    std::vector<int> cls(enc.ids.size(), 0);
    size_t a = 0;
    for (;;) {
        size_t b = std::min(enc.ids.size(), a + kChunkTokens);
        if (!RunWindow(enc.ids, a, b, cls)) return {};
        size_t keepTo = (b == enc.ids.size()) ? b : b - kOverlapTokens;
        if (b == enc.ids.size()) break;
        a = keepTo;
    }

    // place marks at token ends (last label at a position wins, as in the reference)
    std::map<int, wchar_t> markAt;
    const int n = (int)proc.size();
    for (size_t k = 0; k < enc.ids.size(); ++k) {
        auto m = g_id2mark.find(cls[k]);
        if (m == g_id2mark.end()) continue;
        int e = enc.spans[k].second;
        if (e <= enc.spans[k].first || e > n) continue;   // zero-width: no vote
        if (e != n && proc[e] != L' ') continue;          // marks land on word ends only
        markAt[e] = m->second;
    }
    // never double up: if the next char already carries punctuation (digit-kept '.'), drop the mark
    for (auto it = markAt.begin(); it != markAt.end(); ) {
        int e = it->first;
        if (e < n && IsMark(proc[e])) it = markAt.erase(it);
        else ++it;
    }

    std::wstring res;
    res.reserve(proc.size() + markAt.size() * 2 + 8);
    size_t prev = 0;
    for (auto& [e, m] : markAt) {
        res.append(proc, prev, (size_t)e - prev);
        res.push_back(m);
        prev = (size_t)e;
    }
    res.append(proc, prev, std::wstring::npos);

    return Capitalize(res);
}

bool DumpTokens(const std::wstring& text, const std::wstring& outPath)
{
    if (!g_ready) return false;
    std::wstring proc = Preprocess(text);
    Encoded enc;
    Encode(proc, enc);
    HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string line;
    for (size_t k = 0; k < enc.ids.size(); ++k) {
        line = std::to_string(enc.ids[k]) + " " + std::to_string(enc.spans[k].first) + " " +
               std::to_string(enc.spans[k].second) + " " + Utf8(proc.substr(enc.spans[k].first,
                          enc.spans[k].second - enc.spans[k].first)) + "\n";
        DWORD got;
        WriteFile(h, line.data(), (DWORD)line.size(), &got, nullptr);
    }
    CloseHandle(h);
    return true;
}

}
