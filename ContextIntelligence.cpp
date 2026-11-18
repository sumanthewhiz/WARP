#include "framework.h"
#include "ContextIntelligence.h"
#include "ContextInference.h"
#include "EntityKey.h"
#include "EntityStore.h"
#include "Json.h"
#include "Settings.h"
#include "Attention.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <unordered_map>

namespace
{
    using namespace ContextIntelligence;
    using WarpJson::Value;
    using WarpJson::Writer;

    void Unique(std::vector<std::string>& values)
    {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }

    std::string ShortText(const std::string& value, size_t limit = 160)
    {
        auto text = EntityKey::FromUtf8(value);
        if (text.size() > limit) text.resize(limit);
        for (auto& ch : text)
            if (ch < 32 || ch == L'<' || ch == L'>' || ch == L'|') ch = L' ';
        return EntityKey::ToUtf8(text);
    }

    std::vector<std::wstring> Tokens(const std::string& text)
    {
        auto lower = EntityKey::FromUtf8(EntityKey::Key(EntityKey::FromUtf8(text)));
        std::vector<std::wstring> tokens;
        std::wstring word;
        static const std::set<std::wstring> stop = {
            L"the", L"a", L"an", L"my", L"i", L"file", L"files", L"document", L"documents",
            L"find", L"that", L"from", L"in", L"on", L"of", L"and", L"to", L"downloaded",
            L"opened", L"modified", L"yesterday", L"today"
        };
        auto flush = [&] {
            if (!word.empty() && !stop.count(word)) tokens.push_back(word);
            word.clear();
        };
        for (wchar_t ch : lower)
        {
            if (iswalnum(ch) || ch >= 128) word += ch;
            else flush();
        }
        flush();
        return tokens;
    }

    bool QueryMatches(const std::string& query, const Candidate& candidate, const std::string& intent)
    {
        const auto tokens = Tokens(query);
        if (tokens.empty()) return intent != "general";
        const auto hay = EntityKey::FromUtf8(EntityKey::Key(EntityKey::FromUtf8(
            candidate.name + " " + candidate.title + " " + candidate.key)));
        for (const auto& token : tokens)
            if (hay.find(token) == std::wstring::npos) return false;
        return true;
    }

    bool Strong(const EvidenceRelation& relation)
    {
        return relation.quality >= .6 && (relation.kind == "download_origin" ||
            relation.kind == "renamed_from" || relation.kind == "alias_of");
    }

    bool AllowedObject(const Value& object, const std::set<std::string>& allowed)
    {
        if (!object.isObject()) return false;
        std::set<std::string> seen;
        for (const auto& member : object.members())
            if (!allowed.count(member.first) || !seen.insert(member.first).second) return false;
        return true;
    }

    void Strings(Writer& w, const char* name, const std::vector<std::string>& values)
    {
        w.Key(name).BeginArray();
        for (const auto& value : values) w.String(value);
        w.EndArray();
    }
}

namespace ContextIntelligence
{
    Frame Build(const ContextSnapshot& snapshot, const ActivityStore::EvidenceSet& evidence)
    {
        Frame frame;
        frame.startMs = evidence.startMs; frame.endMs = evidence.endMs; frame.asOfMs = evidence.asOfMs;
        frame.watermark = evidence.watermark; frame.available = evidence.available;
        frame.historical = snapshot.historical; frame.partial = evidence.partial;
        frame.truncated = evidence.truncated || snapshot.entitiesTruncated;
        frame.redacted = evidence.privacyRedacted; frame.presence = snapshot.presence;
        frame.coverageJson = ActivityStore::EvidenceJson(evidence, 0);
        frame.aliases = evidence.aliases;
        if (frame.aliases.size() > 4096) { frame.aliases.resize(4096); frame.truncated = true; }
        struct Support
        {
            ObservationEvidence observation;
            int64_t start = 0, end = 0;
            bool eligible = false;
            double quality = 0;
            std::string title, app, action;
        };
        std::map<std::string, Support> supports;
        auto add = [&](const std::string& ref, const ObservationEvidence& e, int64_t start,
                       int64_t end, bool eligible, double quality, const std::string& title,
                       const std::string& app, const std::string& action) {
            if (e.id.empty() || e.redaction != "none" || e.replayed || e.timestampKind == "recency_observation") return;
            if (!std::isfinite(quality)) return;
            quality = (std::max)(0.0, (std::min)(1.0, quality));
            if (e.identityQuality >= 0) quality = (std::min)(quality, e.identityQuality);
            if (e.sourceQuality >= 0) quality = (std::min)(quality, e.sourceQuality);
            if (e.confirmation == "unconfirmed") quality *= .5;
            supports[ref] = { e, (std::max)(start, frame.startMs), (std::min)(end, frame.endMs),
                eligible && quality >= .5, quality, ShortText(title), ShortText(app, 48), action };
        };
        for (const auto& f : evidence.files)
        {
            if (Settings::Instance().IsExcludedPath(f.path) || Settings::Instance().IsExcludedApp(f.sourceExe))
            { frame.redacted = true; continue; }
            const bool explicitOpen = f.action == L"OPEN" &&
                (f.source == L"recent" || f.source == L"mru" || f.source == L"shell");
            const bool qualifiedChange = (f.action == L"MODIFY" || f.action == L"CREATE") &&
                f.evidence.actorQuality >= .5;
            add("file:" + std::to_string(f.id), f.evidence, f.eventMs, f.eventMs + 1,
                explicitOpen || qualifiedChange || f.action == L"DOWNLOAD", f.confidence,
                EntityKey::ToUtf8(EntityKey::BaseName(f.path)), EntityKey::ToUtf8(EntityKey::BaseName(f.sourceExe)),
                EntityKey::Key(f.action));
        }
        for (const auto& f : evidence.focus)
        {
            if (Settings::Instance().IsExcludedApp(f.exePath) || Settings::Instance().IsExcludedTitle(f.windowTitle))
            { frame.redacted = true; continue; }
            add("focus:" + std::to_string(f.id), f.evidence, f.startMs, f.endMs,
                f.activeSecs > 0 && !f.docKey.empty() && f.docResolution != "ambiguous" &&
                f.docResolution != "withheld" && f.docResolution != "unresolved", f.confidence,
                EntityKey::ToUtf8(f.windowTitle), EntityKey::ToUtf8(f.exeName), "foreground");
        }
        for (const auto& b : evidence.browsing)
        {
            if (Settings::Instance().IsExcludedTitle(b.title) || Settings::Instance().IsExcludedApp(b.browser))
            { frame.redacted = true; continue; }
            if (b.id >= 0) add("browse:" + std::to_string(b.id), b.evidence, b.eventMs, b.eventMs + 1,
                !b.url.empty() && b.evidence.resolution != "ambiguous" && b.evidence.resolution != "withheld",
                b.confidence, EntityKey::ToUtf8(b.title), EntityKey::ToUtf8(b.browser), "page_observed");
        }
        for (const auto& e : evidence.episodes)
            if (e.kind == "page" && !e.entityKey.empty() && e.exposureMs > 0)
            {
                if (Settings::Instance().IsExcludedTitle(e.title) || Settings::Instance().IsExcludedApp(EntityKey::FromUtf8(e.appKey)))
                { frame.redacted = true; continue; }
                add("page:" + e.id, e.evidence, e.startMs, e.endMs, true, .7, EntityKey::ToUtf8(e.title), "Browser", "page_observed");
            }

        for (const auto& entity : snapshot.entities)
        {
            if (entity.entityType == "app") continue;
            bool excluded = Settings::Instance().IsExcludedTitle(EntityKey::FromUtf8(entity.title)) ||
                (entity.entityType == "file" && Settings::Instance().IsExcludedPath(EntityKey::FromUtf8(entity.display)));
            if (entity.entityType == "url")
            {
                const auto scheme = entity.display.find("://");
                if (scheme != std::string::npos)
                {
                    const auto end = entity.display.find_first_of("/?#", scheme + 3);
                    excluded = excluded || Settings::Instance().IsExcludedDomain(EntityKey::FromUtf8(
                        entity.display.substr(scheme + 3, end == std::string::npos ? end : end - scheme - 3)));
                }
            }
            if (excluded) { frame.redacted = true; continue; }
            Card card;
            card.key = entity.entityKey; card.type = entity.entityType;
            card.display = entity.display;
            card.resolution = entity.resolution;
            card.engagement = entity.score; card.firstMs = frame.endMs;
            card.workspace = Evidence::WorkspaceFor(card.display, card.type);
            auto refs = entity.evidenceRefs;
            for (const auto& e : evidence.episodes)
                if (e.kind == "page" && e.entityKey == card.key) refs.push_back("page:" + e.id);
            for (const auto& ref : refs)
            {
                const auto it = supports.find(ref);
                if (it == supports.end()) continue;
                const auto& support = it->second;
                card.support.push_back(support.observation.id);
                card.sources.push_back(ref.substr(0, ref.find(':')) + ":" + support.observation.actorMethod +
                    ":" + support.observation.confirmation);
                card.firstMs = (std::min)(card.firstMs, support.start);
                if (support.end >= card.lastMs)
                {
                    card.lastMs = support.end;
                    card.title = support.title;
                    card.app = support.app;
                }
                card.eligible = card.eligible || support.eligible;
                if (support.eligible) card.actions.push_back(support.action);
                card.quality = (std::max)(card.quality, support.quality);
                if (!support.observation.artifactId.empty())
                {
                    if (card.artifactId.empty()) card.artifactId = support.observation.artifactId;
                    else if (card.artifactId != support.observation.artifactId)
                        card.resolution = "ambiguous_reused_path";
                }
            }
            Unique(card.support);
            Unique(card.sources);
            Unique(card.actions);
            if (card.support.empty()) continue; // volatile/legacy identity is not a durable model fact
            if (card.resolution.find("ambiguous") != std::string::npos) card.eligible = false;
            auto prior = std::find_if(frame.cards.begin(), frame.cards.end(), [&](const Card& value) { return value.key == card.key; });
            if (prior == frame.cards.end()) frame.cards.push_back(std::move(card));
            else
            {
                prior->support.insert(prior->support.end(), card.support.begin(), card.support.end());
                prior->actions.insert(prior->actions.end(), card.actions.begin(), card.actions.end());
                prior->sources.insert(prior->sources.end(), card.sources.begin(), card.sources.end());
                Unique(prior->support); Unique(prior->actions); Unique(prior->sources);
                prior->firstMs = (std::min)(prior->firstMs, card.firstMs);
                if (card.lastMs >= prior->lastMs)
                { prior->lastMs = card.lastMs; prior->title = card.title; prior->app = card.app; }
                prior->eligible = prior->eligible || card.eligible;
                prior->quality = (std::max)(prior->quality, card.quality);
                if (prior->artifactId.empty()) prior->artifactId = card.artifactId;
                else if (!card.artifactId.empty() && prior->artifactId != card.artifactId)
                    prior->resolution = "ambiguous_reused_path";
                if (card.resolution.find("ambiguous") != std::string::npos) prior->resolution = card.resolution;
                if (prior->resolution.find("ambiguous") != std::string::npos) prior->eligible = false;
                // Duration is re-derived once from all retained episodes for this key below.
            }
        }
        if (frame.cards.size() > 64)
        {
            std::sort(frame.cards.begin(), frame.cards.end(), [](const Card& a, const Card& b) {
                if (a.eligible != b.eligible) return a.eligible;
                if (a.lastMs != b.lastMs) return a.lastMs > b.lastMs;
                return a.key < b.key;
            });
            frame.cards.resize(64);
            frame.selectionTruncated = frame.partial = true;
        }
        std::sort(frame.cards.begin(), frame.cards.end(), [](const Card& a, const Card& b) { return a.key < b.key; });
        std::map<std::string, size_t> byKey;
        std::set<std::string> knownSupport;
        for (size_t i = 0; i < frame.cards.size(); ++i)
        {
            auto& card = frame.cards[i];
            card.activeMs = card.passiveMs = card.idleMs = card.unknownMs = card.exposureMs = 0;
            std::vector<Attention::Span> attention, idle;
            for (const auto& episode : evidence.episodes)
            {
                if (episode.entityKey != card.key || episode.endMs <= episode.startMs ||
                    !std::binary_search(card.support.begin(), card.support.end(), episode.evidence.id)) continue;
                const double wall = double(episode.endMs - episode.startMs);
                attention.push_back({episode.startMs, episode.endMs, (std::max)(int64_t(0), episode.activeMs)/wall,
                    (std::max)(int64_t(0), episode.passiveMs)/wall});
                idle.push_back({episode.startMs, episode.endMs, (std::max)(int64_t(0), episode.idleMs)/wall, 0});
            }
            for (const auto& span : Attention::Union(attention))
            {
                card.exposureMs += span.end - span.start;
                card.activeMs += (int64_t)std::llround((span.end - span.start)*span.active);
                card.passiveMs += (int64_t)std::llround((span.end - span.start)*span.passive);
            }
            for (const auto& span : Attention::Union(idle))
                card.idleMs += (int64_t)std::llround((span.end - span.start)*span.active);
            card.idleMs = (std::min)(card.idleMs, (std::max)(int64_t(0), card.exposureMs-card.activeMs-card.passiveMs));
            card.unknownMs = (std::max)(int64_t(0), card.exposureMs-card.activeMs-card.passiveMs-card.idleMs);
            if (card.eligible) frame.latestMs = (std::max)(frame.latestMs, card.lastMs);
            card.id = "e" + std::to_string(i + 1); byKey[card.key] = i;
            if (card.type == "url") byKey[EntityKey::UrlKey(EntityKey::FromUtf8(card.display))] = i;
            knownSupport.insert(card.support.begin(), card.support.end());
        }
        for (const auto& relation : evidence.relations)
        {
            if (!byKey.count(relation.from) && !byKey.count(relation.to)) continue;
            bool supported = !relation.support.empty();
            for (const auto& id : relation.support) supported = supported && knownSupport.count(id) != 0;
            if (supported)
            {
                auto qualified = relation;
                for (const auto& support : supports)
                    if (std::find(relation.support.begin(), relation.support.end(), support.second.observation.id) != relation.support.end())
                        qualified.quality = (std::min)(qualified.quality, support.second.eligible ? support.second.quality : .2);
                frame.relations.push_back(std::move(qualified));
            }
            if (frame.relations.size() == 128) { frame.selectionTruncated = frame.partial = true; break; }
        }
        for (const auto& episode : evidence.episodes)
        {
            if (!byKey.count(episode.entityKey) || episode.evidence.redaction != "none" ||
                !knownSupport.count(episode.evidence.id)) continue;
            auto prior = std::find_if(frame.episodes.rbegin(), frame.episodes.rend(), [&](const EpisodeRecord& candidate) {
                return candidate.kind == episode.kind && candidate.entityKey == episode.entityKey &&
                    candidate.windowInstance == episode.windowInstance && candidate.endMs == episode.startMs;
            });
            if (prior != frame.episodes.rend())
            {
                prior->endMs = episode.endMs;
                prior->exposureMs += episode.exposureMs;
                prior->activeMs = prior->activeMs < 0 || episode.activeMs < 0 ? -1 : prior->activeMs + episode.activeMs;
                prior->passiveMs = prior->passiveMs < 0 || episode.passiveMs < 0 ? -1 : prior->passiveMs + episode.passiveMs;
                prior->idleMs = prior->idleMs < 0 || episode.idleMs < 0 ? -1 : prior->idleMs + episode.idleMs;
                prior->unknownMs += episode.unknownMs;
            }
            else if (frame.episodes.size() < 64) frame.episodes.push_back(episode);
            else frame.selectionTruncated = frame.partial = true;
        }

        std::vector<size_t> groups(frame.cards.size());
        std::iota(groups.begin(), groups.end(), 0);
        auto root = [&](size_t i) { while (groups[i] != i) i = groups[i]; return i; };
        auto join = [&](size_t a, size_t b) {
            if (frame.cards[a].eligible && frame.cards[b].eligible) groups[root(b)] = root(a);
        };
        for (size_t i = 0; i < frame.cards.size(); ++i)
            for (size_t j = i + 1; j < frame.cards.size(); ++j)
                if (!frame.cards[i].workspace.empty() && frame.cards[i].workspace == frame.cards[j].workspace)
                    join(i, j);
        for (const auto& relation : frame.relations)
            if (Strong(relation) && byKey.count(relation.from) && byKey.count(relation.to))
                join(byKey[relation.from], byKey[relation.to]);
        std::map<size_t, Task> tasks;
        for (size_t i = 0; i < frame.cards.size(); ++i)
        {
            const auto& card = frame.cards[i];
            if (!card.eligible) continue;
            auto& task = tasks[root(i)];
            task.entities.push_back(card.key);
            task.support.insert(task.support.end(), card.support.begin(), card.support.end());
            if (!task.startMs || card.firstMs < task.startMs) task.startMs = card.firstMs;
            if (card.lastMs >= task.endMs)
            {
                task.endMs = card.lastMs; task.label = card.title;
                if (!card.workspace.empty()) task.workspace = card.workspace;
            }
            if (task.workspace.empty() && !card.workspace.empty()) task.workspace = card.workspace;
        }
        for (auto& entry : tasks)
        {
            auto& task = entry.second;
            Unique(task.entities); Unique(task.support);
            task.id = "task:" + Evidence::Fingerprint(task.workspace.empty() ? task.entities.front() : task.workspace);
            frame.tasks.push_back(std::move(task));
        }
        std::sort(frame.tasks.begin(), frame.tasks.end(), [](const Task& a, const Task& b) {
            return a.endMs != b.endMs ? a.endMs > b.endMs : a.id < b.id;
        });
        if (!frame.historical && !frame.tasks.empty() &&
            frame.endMs - frame.latestMs <= FreshnessMs &&
            (frame.presence.empty() || frame.presence == "active" || frame.presence == "passive"))
            frame.tasks.front().primary = true;
        frame.partial = frame.partial || frame.truncated;
        return frame;
    }

    Frame BuildSearch(const ActivityStore::EvidenceSet& evidence, bool historical, const std::string& presence)
    {
        ContextSnapshot snapshot;
        snapshot.historical = historical; snapshot.presence = presence;
        std::map<std::string, ContextSnapshot::EntityItem> entities;
        auto add = [&](const std::string& key, const std::string& type, const std::wstring& display,
                       const std::string& resolution, const std::string& ref) {
            if (key.empty()) return;
            auto& item = entities[key];
            item.entityKey = key; item.entityType = type; item.display = EntityKey::ToUtf8(display);
            if (type == "file") item.title = EntityKey::ToUtf8(EntityKey::BaseName(display));
            if (item.resolution.empty() || resolution.find("ambiguous") != std::string::npos) item.resolution = resolution;
            item.evidenceRefs.push_back(ref);
        };
        for (const auto& file : evidence.files)
            if (!file.isDir && !EntityStore::IsTransientFileName(file.path))
                add(file.entityKey, "file", file.path, "observed_path", "file:" + std::to_string(file.id));
        for (const auto& focus : evidence.focus)
            if (!focus.docKey.empty())
                add(focus.docKey, focus.docType == "file" ? "file" : "url", focus.docDisplay,
                    focus.docResolution, "focus:" + std::to_string(focus.id));
        for (const auto& page : evidence.browsing)
            add(page.entityKey, "url", page.url, page.evidence.resolution, "browse:" + std::to_string(page.id));
        for (const auto& episode : evidence.episodes)
            if (episode.kind == "page")
                add(episode.entityKey, "url", episode.display, episode.resolution, "page:" + episode.id);
        for (auto& pair : entities) snapshot.entities.push_back(std::move(pair.second));
        return Build(snapshot, evidence);
    }

    std::string Json(const Frame& frame)
    {
        std::string out; Writer w(out);
        w.BeginObject().KV("schema", "warp.task-context.v1").KV("rule_version", RuleVersion)
            .KV("start_ms", frame.startMs).KV("end_ms", frame.endMs).KV("as_of_ms", frame.asOfMs)
            .KV("input_watermark", frame.watermark).KV("available", frame.available)
            .KV("mode", frame.historical ? "interval" : "current").KV("latest_evidence_ms", frame.latestMs)
            .KV("partial", frame.partial).KV("truncated", frame.truncated).KV("privacy_redacted", frame.redacted)
            .KV("selection_truncated", frame.selectionTruncated)
            .KV("presence", frame.presence).Key("coverage").Raw(frame.coverageJson.empty() ? "{}" : frame.coverageJson);
        w.Key("entities").BeginArray();
        for (const auto& card : frame.cards)
        {
            w.BeginObject().KV("id", card.id).KV("entity_key", card.key).KV("kind", card.type)
                .KV("title", card.title).KV("display", card.display).KV("workspace", card.workspace)
                .KV("resolution", card.resolution).KV("eligible", card.eligible)
                .KV("first_ms", card.firstMs).KV("last_ms", card.lastMs).KV("active_ms", card.activeMs)
                .KV("exposure_ms", card.exposureMs).KV("passive_ms", card.passiveMs)
                .KV("idle_ms", card.idleMs).KV("unknown_ms", card.unknownMs).KV("quality", card.quality);
            Strings(w, "evidence_refs", card.support); Strings(w, "observed_actions", card.actions);
            w.EndObject();
        }
        w.EndArray().Key("tasks").BeginArray();
        for (const auto& task : frame.tasks)
        {
            w.BeginObject().KV("id", task.id).KV("label", task.label).KV("status", "hypothesis")
                .KV("primary", task.primary).KV("start_ms", task.startMs).KV("end_ms", task.endMs)
                .KV("workspace", task.workspace).KV("time_semantics", "extent_not_continuous_attention");
            Strings(w, "entities", task.entities); Strings(w, "evidence_refs", task.support);
            w.KV("uncertainty", "Association is not proof of intent; nearby episodes alone do not join tasks.").EndObject();
        }
        w.EndArray().Key("episodes").BeginArray();
        for (const auto& episode : frame.episodes)
        {
            w.BeginObject().KV("entity_key", episode.entityKey).KV("kind", episode.kind)
                .KV("start_ms", episode.startMs).KV("end_ms", episode.endMs).KV("exposure_ms", episode.exposureMs);
            if (episode.activeMs < 0) w.KNull("active_ms"); else w.KV("active_ms", episode.activeMs);
            w.KV("unknown_ms", episode.unknownMs).EndObject();
        }
        w.EndArray().Key("relations").BeginArray();
        for (const auto& relation : frame.relations) Evidence::Write(w, relation);
        w.EndArray().Key("interpretation").BeginObject().KV("status", frame.modelStatus)
            .KV("prompt_version", PromptVersion).KV("ranking_default", "deterministic")
            .Key("model_metadata").Raw(frame.modelMetadata.empty() ? "{}" : frame.modelMetadata)
            .Key("result").Raw(frame.modelJson.empty() ? "null" : frame.modelJson)
            .EndObject().EndObject();
        return out;
    }

    std::string ModelInput(const Frame& frame)
    {
        std::string out; Writer w(out);
        std::map<std::string, std::string> ids, workspaceIds;
        for (const auto& card : frame.cards)
        {
            ids[card.key] = card.id;
            if (card.type == "url") ids[EntityKey::UrlKey(EntityKey::FromUtf8(card.display))] = card.id;
            if (!card.workspace.empty() && !workspaceIds.count(card.workspace))
                workspaceIds[card.workspace] = "w" + std::to_string(workspaceIds.size() + 1);
        }
        w.BeginObject().KV("schema", "warp.model-evidence.v1").KV("rule_version", RuleVersion)
            .KV("start_ms", frame.startMs).KV("end_ms", frame.endMs).KV("as_of_ms", frame.asOfMs)
            .KV("partial", frame.partial).KV("truncated", frame.truncated).KV("privacy_redacted", frame.redacted)
            .KV("mode", frame.historical ? "interval" : "current")
            .Key("coverage").BeginArray();
        const auto coverage = WarpJson::Parse(frame.coverageJson);
        for (const auto& source : coverage.value.get("coverage_summary").items())
            w.BeginObject().KV("source", source.get("source").asString())
                .KV("unknown_ms", source.get("unknown_ms").asInt64()).Key("states").Value(source.get("states")).EndObject();
        w.EndArray().Key("entities").BeginArray();
        for (size_t i = 0; i < frame.cards.size(); ++i)
        {
            const auto& card = frame.cards[i];
            w.BeginObject().KV("id", card.id).KV("kind", card.type).KV("title", ShortText(card.title, 80))
                .KV("app", card.app).KV("workspace", workspaceIds[card.workspace])
                .KV("start_s", (card.firstMs - frame.startMs) / 1000).KV("end_s", (card.lastMs - frame.startMs) / 1000).KV("active_ms", card.activeMs)
                .KV("exposure_ms", card.exposureMs).KV("resolution", card.resolution)
                .KV("passive_ms", card.passiveMs).KV("idle_ms", card.idleMs).KV("unknown_ms", card.unknownMs)
                .KV("eligible", card.eligible).KV("evidence_id", "v" + std::to_string(i + 1))
                .KV("quality_rule_weight", card.quality);
            Strings(w, "observed_actions", card.actions);
            Strings(w, "source_actor_confirmation", card.sources);
            w.KV("supporting_observation_count", (int)card.support.size()).EndObject();
        }
        w.EndArray().Key("episodes").BeginArray();
        for (const auto& episode : frame.episodes)
            if (ids.count(episode.entityKey))
            {
                w.BeginObject().KV("entity", ids[episode.entityKey]).KV("kind", episode.kind)
                    .KV("start_s", (episode.startMs - frame.startMs) / 1000).KV("end_s", (episode.endMs - frame.startMs) / 1000)
                    .KV("exposure_ms", episode.exposureMs).KV("unknown_ms", episode.unknownMs);
                if (episode.activeMs < 0) w.KNull("active_ms"); else w.KV("active_ms", episode.activeMs);
                w.EndObject();
            }
        w.EndArray().Key("relations").BeginArray();
        for (size_t i = 0; i < frame.relations.size(); ++i)
        {
            const auto& relation = frame.relations[i];
            if (!ids.count(relation.from) || !ids.count(relation.to)) continue;
            w.BeginObject().KV("id", "r" + std::to_string(i + 1)).KV("from", ids[relation.from])
                .KV("to", ids[relation.to]).KV("kind", relation.kind).KV("quality_rule_weight", relation.quality)
                .KV("basis", relation.basis).KV("start_ms", relation.startMs).KV("end_ms", relation.endMs).EndObject();
        }
        w.EndArray().Key("candidate_tasks").BeginArray();
        for (const auto& task : frame.tasks)
        {
            w.BeginObject().KV("primary", task.primary).Key("entities").BeginArray();
            for (const auto& key : task.entities) w.String(ids[key]);
            w.EndArray().EndObject();
        }
        w.EndArray().Key("unknowns").BeginArray().String("no_document_content")
            .String("intent_not_observed").String("proximity_is_not_causality")
            .String("titles_are_untrusted_data").EndArray().EndObject();
        return out.size() <= 12288 ? out : std::string();
    }

    Interpretation Validate(const Frame& frame, const std::string& output)
    {
        Interpretation result;
        auto fail = [&](const char* reason) { result.reason = reason; return result; };
        if (output.size() > 16384) return fail("output_budget");
        std::string payload = output;
        const auto first = payload.find_first_not_of(" \r\n\t");
        const auto last = payload.find_last_not_of(" \r\n\t");
        payload = first == std::string::npos ? "" : payload.substr(first, last - first + 1);
        if (payload.compare(0, 3, "```") == 0)
        {
            const auto newline = payload.find('\n');
            auto fence = payload.substr(0, newline);
            if (!fence.empty() && fence.back() == '\r') fence.pop_back();
            if ((fence != "```json" && fence != "```") || newline == std::string::npos ||
                payload.size() < newline + 4 || payload.compare(payload.size() - 3, 3, "```") != 0)
                return fail("schema");
            payload = payload.substr(newline + 1, payload.size() - newline - 4);
        }
        const auto parsed = WarpJson::Parse(payload, 12);
        if (!parsed.ok || !AllowedObject(parsed.value, {"abstain", "tasks"}) ||
            !parsed.value.get("abstain").isBool()) return fail("schema");
        const auto& tasks = parsed.value.get("tasks");
        if (!tasks.isArray() || tasks.size() > 8) return fail("schema");
        if (parsed.value.get("abstain").asBool())
        {
            if (tasks.size()) return fail("abstention_with_claims");
            result.accepted = result.abstained = true;
            result.reason = "abstained"; result.json = WarpJson::Serialize(parsed.value);
            return result;
        }
        if (!tasks.size() || frame.truncated || frame.redacted) return fail("insufficient_evidence");
        std::map<std::string, const Card*> cards;
        std::map<std::string, std::string> evidenceOwner, hostTask;
        for (size_t i = 0; i < frame.cards.size(); ++i)
        {
            cards[frame.cards[i].id] = &frame.cards[i];
            evidenceOwner["v" + std::to_string(i + 1)] = frame.cards[i].id;
        }
        for (const auto& task : frame.tasks)
            for (const auto& key : task.entities) hostTask[key] = task.id;
        auto covered = [&](const Value& refs, const std::string& entity) {
            if (!refs.isArray() || refs.size() == 0 || refs.size() > 24) return false;
            bool found = false;
            for (const auto& ref : refs.items())
            {
                if (!ref.isString() || !evidenceOwner.count(ref.asString())) return false;
                found = found || evidenceOwner[ref.asString()] == entity;
            }
            return found;
        };
        std::set<std::string> claimed;
        for (const auto& task : tasks.items())
        {
            if (!AllowedObject(task, {"entities", "label_entity", "evidence_refs", "roles"}) ||
                !task.get("entities").isArray() || !task.get("roles").isArray() ||
                task.get("entities").size() == 0 || task.get("entities").size() > 24 ||
                !task.get("label_entity").isString()) return fail("schema");
            std::set<std::string> group;
            std::string parent;
            for (const auto& entity : task.get("entities").items())
            {
                if (!entity.isString() || !cards.count(entity.asString())) return fail("unknown_entity");
                const auto& id = entity.asString();
                const auto* card = cards[id];
                if (!card->eligible || !claimed.insert(id).second || !covered(task.get("evidence_refs"), id))
                    return fail("unsupported_entity");
                const auto owner = hostTask[card->key];
                if (owner.empty() || (!parent.empty() && owner != parent)) return fail("unsupported_task_merge");
                parent = owner; group.insert(id);
            }
            if (!group.count(task.get("label_entity").asString())) return fail("unsupported_label");
            std::set<std::string> roles;
            for (const auto& role : task.get("roles").items())
            {
                if (!AllowedObject(role, {"entity", "role", "evidence_refs"}) ||
                    !role.get("entity").isString() || !role.get("role").isString()) return fail("schema");
                const auto id = role.get("entity").asString(), value = role.get("role").asString();
                if (!group.count(id) || !roles.insert(id).second || !covered(role.get("evidence_refs"), id))
                    return fail("unsupported_role");
                if (value != "primary_artifact" && value != "possible_reference" && value != "observed")
                    return fail("unsupported_role");
                if (value == "primary_artifact" && cards[id]->activeMs < 30000)
                    return fail("unsupported_attention");
            }
            if (roles.size() != group.size()) return fail("missing_roles");
        }
        size_t eligible = 0;
        for (const auto& card : frame.cards) if (card.eligible) ++eligible;
        if (claimed.size() != eligible) return fail("missing_evidence");
        result.accepted = true; result.reason = "accepted";
        result.json = WarpJson::Serialize(parsed.value);
        return result;
    }

    void Apply(Frame& frame, const Interpretation& result)
    {
        frame.modelStatus = result.accepted ? result.reason : "rejected:" + result.reason;
        if (!result.accepted) { frame.modelJson.clear(); return; }
        frame.modelJson = result.json;
        if (result.abstained) return;
        const auto parsed = WarpJson::Parse(result.json);
        std::map<std::string, const Card*> cards;
        for (const auto& card : frame.cards) cards[card.id] = &card;
        std::vector<Task> tasks;
        for (const auto& value : parsed.value.get("tasks").items())
        {
            Task task;
            for (const auto& id : value.get("entities").asStringArray())
            {
                const auto& card = *cards.at(id);
                task.entities.push_back(card.key);
                task.support.insert(task.support.end(), card.support.begin(), card.support.end());
                if (!task.startMs || card.firstMs < task.startMs) task.startMs = card.firstMs;
                if (card.lastMs >= task.endMs)
                {
                    task.endMs = card.lastMs;
                    if (!card.workspace.empty()) task.workspace = card.workspace;
                }
                if (task.workspace.empty() && !card.workspace.empty()) task.workspace = card.workspace;
            }
            Unique(task.entities); Unique(task.support);
            task.label = cards.at(value.get("label_entity").asString())->title;
            std::string signature;
            for (const auto& key : task.entities) signature += key + "\n";
            task.id = "model-task:" + Evidence::Fingerprint(signature);
            tasks.push_back(std::move(task));
        }
        std::sort(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) {
            return a.endMs != b.endMs ? a.endMs > b.endMs : a.id < b.id;
        });
        if (!tasks.empty() && !frame.historical && frame.endMs - frame.latestMs <= FreshnessMs)
            tasks.front().primary = true;
        frame.tasks = std::move(tasks);
    }

    std::vector<CandidateFeatures> Score(const Frame& frame, const std::string& query,
                                        const std::string& intent, const std::vector<Candidate>& candidates,
                                        int64_t nowMs)
    {
        std::vector<CandidateFeatures> out;
        std::map<std::string, const Card*> cards;
        for (const auto& card : frame.cards) cards[card.key] = &card;
        std::string fallback;
        if (!frame.available) fallback = "unavailable";
        else if (frame.redacted) fallback = "privacy_redacted";
        else if (frame.truncated) fallback = "truncated";
        else if (frame.tasks.empty()) fallback = "no_qualified_evidence";
        else if (!frame.historical && (frame.latestMs <= 0 || nowMs - frame.latestMs > FreshnessMs))
            fallback = "stale";
        else if (!frame.historical && !frame.presence.empty() && frame.presence != "active" && frame.presence != "passive")
            fallback = "not_present";
        else if (Settings::Instance().IsPaused(nowMs / 1000)) fallback = "paused";
        for (const auto& candidate : candidates)
        {
            CandidateFeatures f; f.key = candidate.key; f.fallback = fallback;
            const Card* card = cards.count(candidate.key) ? cards[candidate.key] : nullptr;
            const ArtifactAlias* identity = nullptr;
            for (const auto& alias : frame.aliases)
                if (alias.pathKey == candidate.key && alias.fromMs <= frame.asOfMs &&
                    (alias.toMs == 0 || alias.toMs > frame.asOfMs) &&
                    (!identity || alias.fromMs > identity->fromMs)) identity = &alias;
            if (card && identity && !card->artifactId.empty() && card->artifactId != identity->artifactId)
            {
                card = nullptr; f.identity = "ambiguous_reused_path";
            }
            if (!card && identity && f.identity != "ambiguous_reused_path")
                for (const auto& possible : frame.cards)
                    if (!possible.artifactId.empty() && possible.artifactId == identity->artifactId)
                    { card = &possible; f.identity = "artifact_alias"; break; }
            if (card)
            {
                f.found = true; f.quality = card->quality;
                if (f.identity == "unseen") f.identity = "observed_path";
            }
            if (!f.fallback.empty()) { out.push_back(std::move(f)); continue; }
            if (Settings::Instance().IsExcludedPath(EntityKey::FromUtf8(candidate.key)) ||
                Settings::Instance().IsExcludedTitle(EntityKey::FromUtf8(candidate.title + " " + candidate.name)))
                f.fallback = "excluded";
            else if (!std::isfinite(candidate.baseScore) || candidate.baseScore < 20 ||
                !QueryMatches(query, candidate, intent)) f.fallback = "query_mismatch";
            else if (f.identity == "ambiguous_reused_path" || (card && !card->eligible))
                f.fallback = "ambiguous_or_unqualified";
            if (!f.fallback.empty()) { out.push_back(std::move(f)); continue; }
            const auto workspace = Evidence::WorkspaceFor(candidate.key, "file");
            for (const auto& task : frame.tasks)
            {
                if (!frame.historical && !task.primary) continue;
                const auto key = card ? card->key : candidate.key;
                const bool member = std::find(task.entities.begin(), task.entities.end(), key) != task.entities.end();
                const bool sameWorkspace = !workspace.empty() && workspace == task.workspace;
                if (!member && !sameWorkspace) continue;
                f.taskFit = (std::max)(f.taskFit, member ? 1.0 : .75);
                f.quality = (std::max)(f.quality, member && card ? card->quality : .6);
                f.reasons.push_back(member ? (frame.historical ? "observed_in_requested_interval" : "current_task_member") :
                    "same_directory_or_repository_association");
                f.support.insert(f.support.end(), task.support.begin(), task.support.end());
                if (member && card) f.activity = 1.0;
            }
            for (const auto& relation : frame.relations)
                if (Strong(relation) && (relation.from == candidate.key || relation.to == candidate.key))
                {
                    if (f.taskFit > 0) f.relation = (std::max)(f.relation, relation.quality);
                    if (relation.kind == "download_origin" && intent == "download")
                    {
                        f.activity = 1; f.taskFit = 1; f.quality = relation.quality;
                        f.reasons.push_back("download_origin_observed_not_reading");
                        f.support.insert(f.support.end(), relation.support.begin(), relation.support.end());
                    }
                }
            if (intent != "general")
            {
                const std::string action = intent == "download" ? "download" : intent == "open" ? "open" : "modify";
                if (!card || std::find(card->actions.begin(), card->actions.end(), action) == card->actions.end())
                { f.taskFit = f.activity = f.relation = 0; f.fallback = "action_not_observed"; }
            }
            Unique(f.reasons); Unique(f.support);
            if (f.support.size() > 16) f.support.resize(16);
            f.eligible = f.taskFit > 0 && !f.support.empty();
            f.delta = f.eligible ? 8.0 * (std::min)(1.0, .7 * f.taskFit + .2 * f.activity + .1 * f.relation) : 0;
            out.push_back(std::move(f));
        }
        return out;
    }

    void Write(Writer& w, const CandidateFeatures& f)
    {
        w.BeginObject().KV("key", f.key).KV("found", f.found).KV("identity", f.identity)
            .KV("eligible", f.eligible).KV("fallback", f.fallback).KV("task_fit", f.taskFit)
            .KV("interval_activity", f.activity).KV("typed_relation", f.relation)
            .KV("evidence_quality", f.quality).KV("score_delta", f.delta)
            .KV("score_semantics", "bounded_additive_to_0_100_base_not_probability");
        Strings(w, "reasons", f.reasons); Strings(w, "evidence_refs", f.support);
        w.EndObject();
    }
}
