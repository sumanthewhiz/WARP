"""
Live smoke-test of the new LlmSummarizer prompt against Qwen3-0.6B.

Runs three representative WARP polish scenarios through the model and
prints the raw output, so we can sanity-check the prompt produces
sensible prose rather than (a) a verbatim copy of the items list,
(b) a verbatim copy of the existing summary, or (c) a leak of the
instruction text.
"""
import sys
from transformers import AutoModelForCausalLM, AutoTokenizer

MODEL_ID = "Qwen/Qwen3-0.6B"


def build_prompt(items, category="all"):
    """Mirror of LlmSummarizer::BuildPrompt (C++) for live testing."""
    sys_msg = (
        "You describe what someone is doing on their computer right now, "
        "based on the apps and windows they have open. "
        "Write 1 to 3 short sentences in plain English, like a coworker "
        "glancing at their screen would describe it. "
        "Mention specific app and document names from the list. "
        "Do not invent anything that is not in the list. "
        "Do not use bullet points, quotation marks, numbered lists, or "
        "any markdown."
    )
    extra = {
        "files":    " Focus on the documents and files they have open.",
        "websites": " Focus on the web pages they are browsing.",
        "apps":     (" Focus on the communication and utility apps they "
                     "are using (email, chat, terminals, media, "
                     "remote desktop)."),
    }
    sys_msg += extra.get(category, "")

    user = "Apps and windows open right now (most-used first):\n"
    for app, title in items[:5]:
        user += f"{app} - {title}\n"
    user += "\nDescribe what they are doing in 1 to 3 sentences:"

    return (
        f"<|im_start|>system\n{sys_msg}<|im_end|>\n"
        f"<|im_start|>user\n{user}<|im_end|>\n"
        f"<|im_start|>assistant\n<think>\n\n</think>\n\n"
    )


SCENARIOS = [
    ("all", [
        ("Outlook", "Inbox - Suman.Ghosh@microsoft.com"),
    ]),
    ("all", [
        ("Visual Studio", "ContextInference.cpp - WARP"),
        ("Edge",          "auth refactor PR #123 - GitHub"),
        ("Microsoft Teams", "Daily Standup"),
    ]),
    ("files", [
        ("Word",  "Q4 Planning Doc.docx"),
        ("Excel", "Budget 2026.xlsx"),
    ]),
    ("apps", [
        ("Outlook", "Inbox - foo@bar.com"),
        ("Microsoft Teams", "Daily Standup"),
        ("Windows Terminal", "PowerShell"),
    ]),
    ("websites", [
        ("Browser", "auth refactor PR #123 - GitHub"),
        ("Browser", "React useEffect docs - reactjs.org"),
    ]),
]


def main():
    print(f"loading {MODEL_ID} ...", flush=True)
    tok   = AutoTokenizer.from_pretrained(MODEL_ID)
    model = AutoModelForCausalLM.from_pretrained(
        MODEL_ID, torch_dtype="auto", device_map="cpu"
    )
    print("loaded\n", flush=True)

    for category, items in SCENARIOS:
        prompt = build_prompt(items, category)
        ids = tok.encode(prompt, return_tensors="pt")
        out = model.generate(
            ids,
            max_new_tokens=96,
            do_sample=False,
            temperature=None, top_p=None, top_k=None,
            pad_token_id=tok.eos_token_id,
        )
        new = out[0][ids.shape[1]:]
        decoded = tok.decode(new, skip_special_tokens=True)
        decoded = decoded.split("<|im_end|>")[0].strip()

        print(f"--- category={category} ---")
        print("INPUT items:", items)
        print("OUTPUT:")
        print(decoded)
        print()


if __name__ == "__main__":
    main()
