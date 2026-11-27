// Post-dictation punctuation restoration for Tiro.
//
// Model: Horizon-Labs/punctuation-restoration-base (Apache-2.0, ModernBERT token
// classifier). Runs on the ONNX Runtime already statically linked for the ASR engine -
// no new dependency, no DLL, no Python. The tokenizer is implemented here (Metaspace
// BPE) because sherpa-onnx exposes no tokenizer API; it reproduces HuggingFace
// token ids and offset mappings exactly (verified against gt_tokens fixtures).
//
// The pass is strictly additive to what the recogniser heard: it strips model-injected
// marks, re-reads the whole utterance once, and re-emits it with punctuation, sentence
// capitalization and pronoun "I". No words are added, changed or dropped, so the
// one-shot span replacement in main.cpp stays honest.

#pragma once
#include <string>

namespace punct {

// Load model_quantized.onnx + tokenizer.json + config.json from modelDir.
// logFn receives status lines; may be called from a background thread.
bool Init(const std::wstring& modelDir, void (*logFn)(const wchar_t*), int threads = 1);
bool Ready();

// Full polish: strip existing marks, restore punctuation, capitalize sentences.
// Returns empty string if not Ready or on internal failure (caller keeps raw text).
std::wstring Polish(const std::wstring& text);

// Debug/test hook: tokenize `text` the way the model sees it and write one line per
// token ("id start end") to outPath, for byte-exact comparison against the Python
// reference tokenizer. Returns false if not Ready.
bool DumpTokens(const std::wstring& text, const std::wstring& outPath);

}
