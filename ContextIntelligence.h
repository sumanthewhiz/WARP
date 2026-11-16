#pragma once

#include "ActivityStore.h"
#include <map>
#include <string>
#include <vector>

struct ContextSnapshot;
namespace WarpJson { class Value; }

namespace ContextIntelligence
{
    constexpr const char* RuleVersion = "task-evidence-1";
    constexpr const char* PromptVersion = "evidence-tasks-1";
    constexpr int64_t FreshnessMs = 300000;

    struct Card
    {
        std::string id, key, type, display, title, app, workspace, artifactId, resolution;
        std::vector<std::string> support, actions, sources;
        int64_t firstMs = 0, lastMs = 0, activeMs = 0, exposureMs = 0, passiveMs = 0, idleMs = 0, unknownMs = 0;
        double quality = 0, engagement = 0;
        bool eligible = false;
    };
    struct Task
    {
        std::string id, label, workspace;
        std::vector<std::string> entities, support;
        int64_t startMs = 0, endMs = 0;
        bool primary = false;
    };
    struct Frame
    {
        int64_t startMs = 0, endMs = 0, asOfMs = 0, watermark = 0, latestMs = 0;
        bool available = false, historical = false, partial = true, truncated = false, redacted = false;
        std::string presence, coverageJson, modelStatus = "disabled", modelJson, modelMetadata;
        std::vector<Card> cards;
        std::vector<Task> tasks;
        std::vector<EvidenceRelation> relations;
        std::vector<EpisodeRecord> episodes;
        std::vector<ArtifactAlias> aliases;
    };
    struct Candidate
    {
        std::string key, name, title;
        double baseScore = 0;
        bool exact = false;
    };
    struct CandidateFeatures
    {
        std::string key, identity = "unseen", fallback;
        bool found = false, eligible = false;
        double taskFit = 0, activity = 0, relation = 0, quality = 0, delta = 0;
        std::vector<std::string> reasons, support;
    };
    struct Interpretation
    {
        bool accepted = false, abstained = false;
        std::string reason, json;
    };

    Frame Build(const ContextSnapshot& snapshot, const ActivityStore::EvidenceSet& evidence);
    std::string Json(const Frame& frame);
    std::string ModelInput(const Frame& frame);
    Interpretation Validate(const Frame& frame, const std::string& output);
    void Apply(Frame& frame, const Interpretation& result);
    std::vector<CandidateFeatures> Score(const Frame& frame, const std::string& query,
                                        const std::string& intent, const std::vector<Candidate>& candidates,
                                        int64_t nowMs);
    void Write(WarpJson::Writer& writer, const CandidateFeatures& features);
}
