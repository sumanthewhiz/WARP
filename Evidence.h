#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace WarpJson
{
    class Writer;
}

// Unknown qualities are -1, not a probability of zero. These are rule outputs,
// never calibrated model confidence.
struct ObservationEvidence
{
    std::string id, sourceInstance, processInstance, foregroundInstance;
    int64_t observedMs = 0, receivedMs = 0, attributionAgeMs = -1, contextAgeMs = -1;
    int64_t resolutionObservedMs = 0, resolutionValidUntilMs = 0;
    int sessionId = -1, supportCount = 1, utcOffsetMinutes = INT32_MIN;
    bool replayed = false;
    std::string timestampKind = "unknown", actorMethod = "unknown";
    std::string confirmation = "unknown", resolution = "unknown", redaction = "none";
    double sourceQuality = -1, actorQuality = -1, identityQuality = -1;
    std::string volumeId, fileId, journalId, sourceRecordId, originApp, cloudUrl;
    std::string artifactId, identityBasis;
    std::string episodeId, windowInstance;
    int revision = 0;
    std::string boundaryReason;
    bool open = false;
};

struct EpisodeRecord
{
    std::string id, kind, entityKey, appKey, windowInstance, processInstance;
    std::wstring display, title;
    std::string resolution, state, boundaryReason;
    int revision = 0;
    int64_t startMs = 0, endMs = 0, observedMs = 0, receivedMs = 0;
    int64_t activeMs = -1, passiveMs = -1, idleMs = -1, unknownMs = 0, exposureMs = 0;
    bool open = false;
    ObservationEvidence evidence;
};

struct SourceCoverage
{
    std::string source, instance, state = "unknown", reason;
    int64_t startMs = 0, endMs = 0, receivedMs = 0;
    uint64_t dropped = 0;
};

struct ArtifactAlias
{
    std::string pathKey, artifactId, physicalId, basis, evidenceId;
    int64_t fromMs = 0, toMs = 0;
};

namespace Evidence
{
    constexpr int CheckpointSeconds = 30;
    constexpr const char* RuleVersion = "evidence-1";
    std::string NewId(const char* prefix);
    std::string FileSha256(const std::wstring& path);
    std::string Fingerprint(const std::string& value);
    const std::string& RuntimeId();
    int64_t NowMs();
    void Prepare(ObservationEvidence& e);
    std::string Json(const ObservationEvidence& e);
    ObservationEvidence Parse(const std::string& json);
    void Write(WarpJson::Writer& w, const ObservationEvidence& e);
    void Write(WarpJson::Writer& w, const EpisodeRecord& e);
    void Write(WarpJson::Writer& w, const SourceCoverage& c);
} // namespace Evidence
