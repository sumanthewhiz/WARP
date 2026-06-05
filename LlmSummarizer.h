#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>

// =====================================================================
//  LlmSummarizer
//
//  Small-LLM "brain" layer that takes the cluster->theme topic-hint
//  feed (per-cluster themes + verbs + titles + app names) produced by
//  the BGE-small / granite / MiniLM clustering pipeline and writes
//  the user-facing natural-prose summary using `Qwen3-0.6B`.
//
//  Architecture
//  ============
//  When this brain layer is loaded and a generation call succeeds,
//  its output OVERWRITES `ContextSnapshot::summary` (and the per-facet
//  variants) so there is exactly one user-facing summary string per
//  facet -- the LLM writes it directly.  The algorithmic
//  cluster->theme summary that the snapshot composer assembled first
//  is passed into the prompt as a topic hint to keep the model
//  grounded, but it is not exposed as a separate field.
//
//  When the brain is absent / fails / times out / produces an
//  ungrounded output, the snapshot keeps the algorithmic
//  cluster->theme summary that was already in `summary` -- graceful
//  degradation, nothing regresses.
//
//  (Prior versions of this header described a "polishing" stage that
//  emitted a separate `summaryPolished` field.  That field was
//  removed in v5.13 once the LLM became the canonical writer and the
//  polished alias was always identical to `summary`.)
//
//  Grounding
//  =========
//  The LLM only sees what we explicitly put in the prompt
//  (clean titles, app friendly names, the candidate summary lines).
//  After generation we validate that the output is plausible:
//    * each line is short (<= ~120 chars)
//    * the line count is 1..3
//    * none of the output is the raw prompt template
//  If validation fails, the LLM output is rejected and the
//  algorithmic summary stands alone.
//
//  Dependency
//  ==========
//  Uses `Microsoft.ML.OnnxRuntimeGenAI` (separate NuGet from the
//  embedding model's `Microsoft.ML.OnnxRuntime`).  ORT-GenAI ships
//  its own BPE tokenizer + KV-cache management + greedy/sampling
//  loop, so we don't need to roll any of that ourselves.
//
//  The model files are NOT committed to the repo.  Build-time CI
//  downloads them from HuggingFace; the user can also drop them
//  into `<exe>\models\qwen\` manually.  Expected layout:
//
//      models/qwen/
//          genai_config.json
//          model.onnx
//          model.onnx.data        (external weights -- typical for
//                                  quantized Qwen3 packaging)
//          tokenizer.json
//          tokenizer_config.json
//          special_tokens_map.json
//          ...
//
//  When the directory or any required file is missing, Init()
//  returns false and Polish() becomes a no-op (returns the input
//  unchanged).  No errors propagate -- all failures degrade
//  gracefully.
//
//  Cost
//  ====
//  ~250 MB on disk (Q4 quantization).  Per-snapshot CPU cost
//  ~500 ms - 2 s for 50 generated tokens, with a hard 3 s timeout.
//  The 60-sec refresh interval has plenty of headroom.
// =====================================================================

// Activity items the polisher needs in order to ground its output.
// Populated from ContextSnapshot::items by the caller.
struct LlmActivityItem
{
    std::string app;          // friendly name, e.g. "Visual Studio"
    std::string title;        // cleaned title, e.g. "ContextInference.cpp - WARP"
    std::string rawTitle;     // full raw title (extra context for the LLM)
    int         focusSeconds = 0;
    int         pct          = 0;
};

class LlmSummarizer
{
public:
    LlmSummarizer();
    ~LlmSummarizer();

    // Load the model from `<modelsDir>/qwen/`.  Returns true on
    // success; false (with side-effect cleanup) when files are
    // missing or initialization fails.  Safe to call once at
    // startup; calling again rebuilds the session.  Pass an empty
    // string to skip initialization entirely.
    bool Init(const std::wstring& modelsDir = L"");

    // Tear down the underlying session.  Safe to call multiple
    // times.  Destructor calls this automatically.
    void Stop();

    bool IsLoaded() const { return m_ready.load(); }

    // Name reported in the JSON snapshot, e.g.
    // "qwen3-0.6b" or "(not loaded)".
    std::string ModelName() const;

    // Take the existing structured summary (3 cluster lines for the
    // combined view, or 1-3 per-category lines) plus the activity
    // items, and produce a polished version.
    //
    //   * `existing`     -- the template-composed summary lines.
    //                       Used as a fallback if polishing fails;
    //                       also fed into the prompt as a starting
    //                       point so the LLM doesn't have to
    //                       re-cluster from scratch.
    //   * `items`        -- the per-app breakdown the snapshot
    //                       already carries.  Provides grounding
    //                       facts.
    //   * `category`     -- "all" / "files" / "websites" / "apps" --
    //                       narrows the prompt instruction.
    //
    // Returns the polished summary (1-3 lines) on success, or an
    // empty vector if polishing failed / model not loaded / output
    // failed grounding validation.
    std::vector<std::string> Polish(
        const std::vector<std::string>& existing,
        const std::vector<LlmActivityItem>& items,
        const std::string& category) const;

private:
    // Opaque PIMPL -- onnxruntime-genai types only included in .cpp.
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    std::atomic<bool>     m_ready{ false };
    mutable std::mutex    m_genMutex;   // serialize Polish() calls
    std::string           m_modelName;
};
