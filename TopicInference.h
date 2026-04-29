#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "BertTokenizer.h"

// Forward-declare ONNX Runtime types to avoid leaking the header.
namespace Ort { struct Env; struct Session; struct SessionOptions; struct MemoryInfo; }

class ActivityDatabase;

struct TopicResult
{
    int64_t     timestamp = 0;       // When this inference was produced
    std::string topics[3];           // Top 3 deduced semantic topic areas
    double      coveragePct = 0.0;   // What % of activities these topics cover
    int         activityCount = 0;   // Total activities in the window
};

// ------------------------------------------------------------------
// Semantic topic inference using a local MiniLM sentence-embedding
// model (all-MiniLM-L6-v2 via ONNX Runtime).
//
// How it works:
//   1. A curated set of ~50 candidate topic labels is embedded once
//      at startup (e.g. "C++ Software Development", "Email & Messaging").
//   2. Every 5 min, all activities from the last 15 min are gathered.
//      Each activity's descriptive text is embedded with the same model.
//   3. Each activity is assigned to the topic whose embedding has the
//      highest cosine similarity to the activity embedding.
//   4. A greedy set-cover selects the top 3 topics that collectively
//      cover >= 90 % of activities.
//
// The model genuinely *understands* meaning: "reviewing John's PR"
// matches "Code Review" even without keyword overlap, because both
// map to nearby points in the 384-dimensional semantic space.
// ------------------------------------------------------------------
class TopicInference
{
public:
    TopicInference();
    ~TopicInference();

    // Init loads the ONNX model + vocab from the given directory.
    // modelsDir should contain "minilm.onnx" and "vocab.txt".
    bool Init(const std::wstring& modelsDir);

    void Start(ActivityDatabase* db);
    void Stop();

    void RunOnce();
    std::string GetRecentContext();
    void ClearHistory();

private:
    ActivityDatabase* m_db = nullptr;
    std::thread       m_thread;
    std::atomic<bool> m_running{ false };
    HANDLE            m_stopEvent = nullptr;

    // ONNX Runtime objects (opaque pointers — allocated in Init, freed in dtor)
    Ort::Env*            m_ortEnv     = nullptr;
    Ort::Session*        m_ortSession = nullptr;
    Ort::SessionOptions* m_ortOpts    = nullptr;
    Ort::MemoryInfo*     m_ortMemInfo = nullptr;
    bool                 m_modelReady = false;

    // Tokenizer
    BertTokenizer m_tokenizer;

    // Pre-computed topic candidate embeddings  (topic label -> 384-d vector)
    struct TopicCandidate
    {
        std::string label;
        std::vector<float> embedding;  // 384 floats
    };
    std::vector<TopicCandidate> m_topicCandidates;

    // Stored results
    std::vector<TopicResult> m_results;
    std::mutex               m_resultsMutex;

    void TimerLoop();
    TopicResult DeduceTopics();

    // Embed a single sentence.  Returns a 384-d vector.
    std::vector<float> Embed(const std::string& text);

    // Cosine similarity between two vectors.
    static float CosineSim(const std::vector<float>& a, const std::vector<float>& b);

    // Find the best matching topic label for an embedding.
    int BestTopic(const std::vector<float>& emb) const;

    // Build the list of candidate topic labels.
    static std::vector<std::string> GetCandidateLabels();

    // Helpers
    static std::string WideToUtf8(const std::wstring& w);
    static std::string EscapeJson(const std::string& s);
};
