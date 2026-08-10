"""
Live smoke-test of the LlmSummarizer topic-first prompt against Qwen3-0.6B.

The polished summaries we ship must capture WHAT the user is working on
(topic / subject), not which apps are open.  Reference good outputs the
prompt is designed to reproduce:
  - "User is reading about indexer reliability in their emails and chats
     (across Outlook and M365 Copilot)"
  - "User is exploring various websites related to indexing, rollout and
     feature controls"

`build_prompt_v2` below mirrors the C++ `LlmSummarizer::BuildPrompt`
(LlmSummarizer.cpp) -- any structural change to the C++ prompt must be
mirrored here so this script remains a faithful regression test.

Run once: `pip install transformers torch` then `python smoke_test_polish.py`.
"""
import sys
from transformers import AutoModelForCausalLM, AutoTokenizer

MODEL_ID = "Qwen/Qwen3-0.6B"


def build_prompt_v2(items, existing, category="all"):
    """Topic-first prompt: lead with subject, apps as context."""
    sys_msg = (
        "You read a snapshot of the window titles on someone's screen "
        "and write 1 to 3 short sentences describing WHAT THEY ARE "
        "WORKING ON -- the subject or topic of their work, not which "
        "apps they have open.\n\n"
        "Style rules:\n"
        "- Start with \"User is\" (or \"They are\") followed by what "
        "they are doing.\n"
        "- Lead with the TOPIC. Mention apps only briefly at the end, "
        "e.g. \"...in their emails and chats\" or \"...across Outlook "
        "and Teams\".\n"
        "- If multiple titles share a theme, capture that theme; do not "
        "list every window.\n"
        "- Do NOT say \"is open\", \"is running\", \"is using\".\n"
        "- Plain English. No bullets, no numbered lists, no markdown, "
        "no quotes around the output.\n\n"
        "Examples of the target style:\n"
        "  User is reading about indexer reliability across their "
        "emails and chats (Outlook, Microsoft Teams).\n"
        "  User is reviewing the auth refactor PR in their browser "
        "and editing the related source file in Visual Studio.\n"
        "  User is exploring various websites about React hooks and "
        "state management."
    )
    extra = {
        "files":    " The snapshot covers documents and files only.",
        "websites": " The snapshot covers web pages only.",
        "apps":     " The snapshot covers communication and utility apps only.",
    }
    sys_msg += extra.get(category, "")

    user = "Window titles on the user's screen (most-used first):\n"
    for app, title in items[:5]:
        user += f"- \"{title}\" ({app})\n"

    if existing:
        user += "\nTopic hint already extracted from the titles "
        user += "(use as inspiration, do not copy verbatim):\n"
        for line in existing:
            user += f"  {line}\n"

    user += "\nWrite 1-3 sentences describing what they are working on:"

    return (
        f"<|im_start|>system\n{sys_msg}<|im_end|>\n"
        f"<|im_start|>user\n{user}<|im_end|>\n"
        f"<|im_start|>assistant\n<think>\n\n</think>\n\n"
    )


# Scenarios mirroring the user's reported bad cases.
SCENARIOS = [
    # 1. Outlook + M365 Copilot, generic-ish titles
    ("apps", [
        ("Outlook", "Inbox - Suman.Ghosh@microsoft.com"),
        ("M365 Copilot", "Search Indexer Reliability - status update"),
    ], ["Reading about Indexer Reliability (in Outlook & M365 Copilot)"]),

    # 2. Many browser tabs on related topics
    ("websites", [
        ("Browser", "Search Indexer Performance - Wiki"),
        ("Browser", "Indexer Rollout Plan - SharePoint"),
        ("Browser", "Feature Controls - Confluence"),
        ("Browser", "Indexer Reliability Dashboard"),
        ("Browser", "Search Platform Roadmap"),
    ], ["Exploring Indexer Rollout (across 5 browser tabs)"]),

    # 3. Mixed work: code + PR + standup
    ("all", [
        ("Visual Studio",  "ContextInference.cpp - WARP"),
        ("Edge",           "auth refactor PR #123 - GitHub"),
        ("Microsoft Teams","Daily Standup"),
    ], ["Working on auth refactor (across Visual Studio & Edge)",
        "Discussing Daily Standup in Microsoft Teams"]),

    # 4. Generic: just inbox -- nothing to synthesize topic from
    ("apps", [
        ("Outlook", "Inbox - Suman.Ghosh@microsoft.com"),
    ], ["Checking email in Outlook"]),

    # 5. File work on related docs
    ("files", [
        ("Word",  "Indexer Reliability Plan v3.docx"),
        ("Excel", "Indexer SLO tracking.xlsx"),
        ("PowerPoint", "Indexer Reliability Review - Q4.pptx"),
    ], ["Working on Indexer Reliability (across Word, Excel, PowerPoint)"]),
]


def main():
    print(f"loading {MODEL_ID} ...", flush=True)
    tok   = AutoTokenizer.from_pretrained(MODEL_ID)
    model = AutoModelForCausalLM.from_pretrained(
        MODEL_ID, torch_dtype="auto", device_map="cpu"
    )
    print("loaded\n", flush=True)

    for i, (category, items, existing) in enumerate(SCENARIOS, 1):
        prompt = build_prompt_v2(items, existing, category)
        ids = tok(prompt, return_tensors="pt")
        out = model.generate(
            ids.input_ids,
            attention_mask=ids.attention_mask,
            max_new_tokens=96,
            do_sample=False,
            temperature=None, top_p=None, top_k=None,
            pad_token_id=tok.eos_token_id,
        )
        new = out[0][ids.input_ids.shape[1]:]
        decoded = tok.decode(new, skip_special_tokens=True)
        decoded = decoded.split("<|im_end|>")[0].strip()

        print(f"=== scenario {i} (category={category}) ===")
        print("ITEMS:    ", items)
        print("EXISTING: ", existing)
        print("OUTPUT:")
        for line in decoded.splitlines():
            print(f"  {line}")
        print()


if __name__ == "__main__":
    main()
