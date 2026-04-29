#include "framework.h"
#include "TopicInference.h"
#include "ActivityDatabase.h"

// ONNX Runtime C++ API
#include <onnxruntime_cxx_api.h>

#include <ctime>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <numeric>

// Interval between inference runs (5 minutes).
static const DWORD TOPIC_INTERVAL_MS = 5 * 60 * 1000;
// Activity window (15 minutes).
static const int64_t TOPIC_WINDOW_SECS = 15 * 60;
// Embedding dimension for all-MiniLM-L6-v2.
static const int EMBED_DIM = 384;
// Max token sequence length.
static const int MAX_SEQ_LEN = 128;

// =====================================================================
//  Candidate topic labels — the semantic "vocabulary" of topics.
//  Each will be embedded once at startup.  Activities are matched
//  to the nearest label via cosine similarity in embedding space.
// =====================================================================
std::vector<std::string> TopicInference::GetCandidateLabels()
{
    return {
        // Software engineering
        "C and C++ software development and programming",
        "C# and .NET software development",
        "Java and Kotlin software development",
        "Python programming and scripting",
        "JavaScript and TypeScript web development",
        "Go programming language development",
        "Rust systems programming",
        "Ruby programming and Rails development",
        "PHP web development",
        "Swift and iOS mobile app development",
        "Android mobile app development",
        "Web frontend development with HTML CSS and frameworks",
        "Backend server and API development",
        "Database queries and SQL development",
        "DevOps, CI/CD pipelines, and deployment",
        "Containers and Docker and Kubernetes",
        "Cloud infrastructure and platform management",
        "Source control and Git version management",
        "Code review and pull request review",
        "Debugging and troubleshooting software issues",
        "Software testing and quality assurance",
        "Build and compilation processes",
        "Software architecture and system design",
        "API design and integration development",
        "Machine learning and AI model development",
        "Data science and statistical analysis",

        // Productivity & communication
        "Email reading and writing correspondence",
        "Team chat and instant messaging",
        "Video conferencing and online meetings",
        "Calendar management and scheduling",
        "Project management and task tracking",
        "Note-taking and knowledge management",
        "Document writing and word processing",
        "Spreadsheet work and data analysis",
        "Presentation creation and editing",
        "PDF document reading and review",

        // Research & learning
        "Technical research and documentation reading",
        "Online learning courses and tutorials",
        "Academic research and paper reading",
        "Technology news and community discussion",

        // Design & media
        "User interface and UX design",
        "Graphic design and image editing",
        "Video editing and multimedia production",
        "3D modeling and animation",

        // System & tools
        "Terminal and command line usage",
        "File management and organization",
        "System administration and configuration",
        "Network monitoring and debugging",

        // Other
        "Music listening and entertainment",
        "Social media browsing",
        "Web browsing and general internet usage",
        "Professional networking and career",
    };
}

// =====================================================================
TopicInference::TopicInference()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

TopicInference::~TopicInference()
{
    Stop();

    delete m_ortSession;
    delete m_ortOpts;
    delete m_ortMemInfo;
    delete m_ortEnv;

    if (m_stopEvent) CloseHandle(m_stopEvent);
}

// =====================================================================
//  Init — load model, vocab, pre-embed topic candidates
// =====================================================================
bool TopicInference::Init(const std::wstring& modelsDir)
{
    // Load tokenizer vocabulary
    std::string vocabPath = WideToUtf8(modelsDir) + "\\vocab.txt";
    if (!m_tokenizer.Load(vocabPath))
        return false;

    // Build ONNX model path
    std::wstring modelPath = modelsDir + L"\\minilm.onnx";

    try
    {
        m_ortEnv  = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "WarpTopicInference");
        m_ortOpts = new Ort::SessionOptions();
        m_ortOpts->SetIntraOpNumThreads(2);
        m_ortOpts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        m_ortSession = new Ort::Session(*m_ortEnv, modelPath.c_str(), *m_ortOpts);
        m_ortMemInfo = new Ort::MemoryInfo(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));
    }
    catch (const Ort::Exception&)
    {
        return false;
    }

    m_modelReady = true;

    // Pre-embed all topic candidate labels
    auto labels = GetCandidateLabels();
    m_topicCandidates.reserve(labels.size());
    for (const auto& label : labels)
    {
        TopicCandidate tc;
        tc.label = label;
        tc.embedding = Embed(label);
        if (!tc.embedding.empty())
            m_topicCandidates.push_back(std::move(tc));
    }

    return !m_topicCandidates.empty();
}

// =====================================================================
//  Sentence embedding with MiniLM
// =====================================================================
std::vector<float> TopicInference::Embed(const std::string& text)
{
    if (!m_modelReady || !m_ortSession)
        return {};

    // Tokenize
    std::vector<int64_t> inputIds = m_tokenizer.Encode(text, MAX_SEQ_LEN);
    int64_t seqLen = static_cast<int64_t>(inputIds.size());

    // Attention mask (1 for real tokens, 0 for padding)
    std::vector<int64_t> attentionMask(seqLen, 1);

    // Token type IDs (all zeros for single-sentence)
    std::vector<int64_t> tokenTypeIds(seqLen, 0);

    // Pad to MAX_SEQ_LEN for consistent tensor shape
    while ((int64_t)inputIds.size() < MAX_SEQ_LEN)
    {
        inputIds.push_back(m_tokenizer.PadId());
        attentionMask.push_back(0);
        tokenTypeIds.push_back(0);
    }

    // Create tensors
    std::array<int64_t, 2> shape = { 1, MAX_SEQ_LEN };

    Ort::Value inputIdsTensor = Ort::Value::CreateTensor<int64_t>(
        *m_ortMemInfo, inputIds.data(), inputIds.size(),
        shape.data(), shape.size());

    Ort::Value attMaskTensor = Ort::Value::CreateTensor<int64_t>(
        *m_ortMemInfo, attentionMask.data(), attentionMask.size(),
        shape.data(), shape.size());

    Ort::Value tokTypeTensor = Ort::Value::CreateTensor<int64_t>(
        *m_ortMemInfo, tokenTypeIds.data(), tokenTypeIds.size(),
        shape.data(), shape.size());

    // Run inference
    const char* inputNames[]  = { "input_ids", "attention_mask", "token_type_ids" };
    const char* outputNames[] = { "last_hidden_state" };

    std::vector<Ort::Value> inputTensors;
    inputTensors.push_back(std::move(inputIdsTensor));
    inputTensors.push_back(std::move(attMaskTensor));
    inputTensors.push_back(std::move(tokTypeTensor));

    std::vector<Ort::Value> outputTensors;
    try
    {
        outputTensors = m_ortSession->Run(
            Ort::RunOptions{ nullptr },
            inputNames, inputTensors.data(), 3,
            outputNames, 1);
    }
    catch (const Ort::Exception&)
    {
        return {};
    }

    // Output shape: [1, seqLen, 384]
    // Mean-pool over the real tokens (where attention_mask == 1).
    const float* rawOutput = outputTensors[0].GetTensorData<float>();

    std::vector<float> embedding(EMBED_DIM, 0.0f);
    int realTokens = 0;
    for (int t = 0; t < MAX_SEQ_LEN; ++t)
    {
        if (attentionMask[t] == 0) continue;
        realTokens++;
        for (int d = 0; d < EMBED_DIM; ++d)
            embedding[d] += rawOutput[t * EMBED_DIM + d];
    }

    if (realTokens > 0)
    {
        for (int d = 0; d < EMBED_DIM; ++d)
            embedding[d] /= static_cast<float>(realTokens);
    }

    // L2-normalize for cosine similarity via dot product
    float norm = 0.0f;
    for (int d = 0; d < EMBED_DIM; ++d)
        norm += embedding[d] * embedding[d];
    norm = std::sqrt(norm);
    if (norm > 1e-9f)
    {
        for (int d = 0; d < EMBED_DIM; ++d)
            embedding[d] /= norm;
    }

    return embedding;
}

float TopicInference::CosineSim(const std::vector<float>& a,
                                const std::vector<float>& b)
{
    if (a.size() != b.size() || a.empty()) return 0.0f;
    // Since both are L2-normalised, cosine = dot product
    float dot = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
        dot += a[i] * b[i];
    return dot;
}

int TopicInference::BestTopic(const std::vector<float>& emb) const
{
    int best = -1;
    float bestSim = -1.0f;
    for (int i = 0; i < (int)m_topicCandidates.size(); ++i)
    {
        float s = CosineSim(emb, m_topicCandidates[i].embedding);
        if (s > bestSim)
        {
            bestSim = s;
            best = i;
        }
    }
    return best;
}

// =====================================================================
//  Timer loop
// =====================================================================
void TopicInference::Start(ActivityDatabase* db)
{
    if (m_running) return;
    m_db = db;
    m_running = true;
    ResetEvent(m_stopEvent);
    m_thread = std::thread(&TopicInference::TimerLoop, this);
}

void TopicInference::Stop()
{
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);
    if (m_thread.joinable()) m_thread.join();
}

void TopicInference::TimerLoop()
{
    RunOnce();

    while (m_running)
    {
        DWORD result = WaitForSingleObject(m_stopEvent, TOPIC_INTERVAL_MS);
        if (result == WAIT_OBJECT_0 || !m_running)
            break;
        RunOnce();
    }
}

void TopicInference::RunOnce()
{
    if (!m_db || !m_modelReady) return;

    TopicResult result = DeduceTopics();

    std::lock_guard<std::mutex> lock(m_resultsMutex);
    m_results.push_back(result);
    if (m_results.size() > 288)
        m_results.erase(m_results.begin());
}

// =====================================================================
//  Core topic deduction — embed activities, match to topics
// =====================================================================
TopicResult TopicInference::DeduceTopics()
{
    TopicResult result;
    result.timestamp = static_cast<int64_t>(std::time(nullptr));

    // Gather all activities from the last 15 minutes
    auto files  = m_db->QueryFilesCustomSeconds(TOPIC_WINDOW_SECS);
    auto apps   = m_db->QueryAppLaunchesCustomSeconds(TOPIC_WINDOW_SECS);
    auto browse = m_db->QueryBrowsingCustomSeconds(TOPIC_WINDOW_SECS);
    auto focus  = m_db->QueryAppFocusCustomSeconds(TOPIC_WINDOW_SECS);

    // Build descriptive text for each activity
    std::vector<std::string> activityTexts;

    for (const auto& f : files)
    {
        std::string text = "File " + WideToUtf8(f.action) + ": " + WideToUtf8(f.path);
        activityTexts.push_back(std::move(text));
    }
    for (const auto& a : apps)
    {
        std::string text = "Launched application " + WideToUtf8(a.exeName);
        activityTexts.push_back(std::move(text));
    }
    for (const auto& b : browse)
    {
        std::string text = WideToUtf8(b.title);
        if (!b.url.empty())
            text += " " + WideToUtf8(b.url);
        activityTexts.push_back(std::move(text));
    }
    for (const auto& f : focus)
    {
        std::string text = "Working in " + WideToUtf8(f.exeName) +
                           ": " + WideToUtf8(f.windowTitle);
        activityTexts.push_back(std::move(text));
    }

    int totalActs = static_cast<int>(activityTexts.size());
    result.activityCount = totalActs;

    if (totalActs == 0)
    {
        result.topics[0] = "(no recent activity)";
        result.coveragePct = 0.0;
        return result;
    }

    // Embed each activity and find its best matching topic
    std::vector<int> actTopicIdx(totalActs, -1);

    for (int i = 0; i < totalActs; ++i)
    {
        auto emb = Embed(activityTexts[i]);
        if (!emb.empty())
            actTopicIdx[i] = BestTopic(emb);
    }

    // Count how many activities map to each topic
    std::unordered_map<int, int> topicCount;
    for (int i = 0; i < totalActs; ++i)
    {
        if (actTopicIdx[i] >= 0)
            topicCount[actTopicIdx[i]]++;
    }

    // Greedy set-cover: pick topics covering the most uncovered activities
    std::unordered_set<int> coveredActs;
    std::string topTopics[3];
    int topicCountSelected = 0;

    for (int pass = 0; pass < 3; ++pass)
    {
        int bestIdx = -1;
        int bestNewCov = 0;

        for (const auto& kv : topicCount)
        {
            int newCov = 0;
            for (int ai = 0; ai < totalActs; ++ai)
            {
                if (coveredActs.count(ai)) continue;
                if (actTopicIdx[ai] == kv.first)
                    newCov++;
            }
            if (newCov > bestNewCov)
            {
                bestNewCov = newCov;
                bestIdx = kv.first;
            }
        }

        if (bestIdx < 0 || bestNewCov == 0)
            break;

        topTopics[topicCountSelected++] = m_topicCandidates[bestIdx].label;

        for (int ai = 0; ai < totalActs; ++ai)
        {
            if (actTopicIdx[ai] == bestIdx)
                coveredActs.insert(ai);
        }
    }

    double coverage = (totalActs > 0)
        ? (static_cast<double>(coveredActs.size()) / totalActs * 100.0)
        : 0.0;

    for (int i = 0; i < 3; ++i)
        result.topics[i] = (i < topicCountSelected) ? topTopics[i] : "";

    result.coveragePct = coverage;
    return result;
}

// =====================================================================
//  Query API
// =====================================================================
std::string TopicInference::GetRecentContext()
{
    std::lock_guard<std::mutex> lock(m_resultsMutex);

    std::ostringstream oss;
    oss << "{\"recent_context\":{";

    if (!m_results.empty())
    {
        const TopicResult& latest = m_results.back();

        oss << "\"timestamp\":" << latest.timestamp
            << ",\"activity_count\":" << latest.activityCount
            << ",\"coverage_pct\":" << latest.coveragePct
            << ",\"topics\":[";

        bool first = true;
        for (int i = 0; i < 3; ++i)
        {
            if (latest.topics[i].empty()) continue;
            if (!first) oss << ",";
            first = false;
            oss << "\"" << EscapeJson(latest.topics[i]) << "\"";
        }
        oss << "]";
        oss << ",\"history_count\":" << m_results.size();
        oss << ",\"model\":\"all-MiniLM-L6-v2\"";
    }
    else
    {
        oss << "\"timestamp\":0,\"activity_count\":0,\"coverage_pct\":0"
            << ",\"topics\":[],\"history_count\":0"
            << ",\"model\":\"all-MiniLM-L6-v2\"";
    }

    oss << "}}";
    return oss.str();
}

void TopicInference::ClearHistory()
{
    std::lock_guard<std::mutex> lock(m_resultsMutex);
    m_results.clear();
}

// =====================================================================
//  Helpers
// =====================================================================
std::string TopicInference::WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

std::string TopicInference::EscapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}
