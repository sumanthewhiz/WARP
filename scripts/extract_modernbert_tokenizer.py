"""
Extracts a flat vocab.txt + merges.txt from a HuggingFace tokenizer.json so
the WARP C++ ModernBertTokenizer can load it without a JSON dependency.

Output format (UTF-8, LF endings):
  vocab.txt   one byte-level-encoded token per line; line N (0-indexed) = ID N
  merges.txt  one merge per line, space-separated, in HF rank order

Special tokens (added_tokens, not part of the main BPE vocab) are written as
a small constant header `special_tokens.txt` with KEY=ID lines.

Usage:
    python extract_modernbert_tokenizer.py <tokenizer.json> <out_dir>
"""
import json
import sys
from pathlib import Path


def main(tokenizer_json: str, out_dir: str) -> int:
    src = json.load(open(tokenizer_json, encoding="utf-8"))
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    model = src["model"]
    if model.get("type") != "BPE":
        print(f"unexpected model.type: {model.get('type')!r}", file=sys.stderr)
        return 2

    vocab = model["vocab"]
    merges = model["merges"]
    sorted_vocab = sorted(vocab.items(), key=lambda kv: kv[1])

    # Sanity check: IDs must be a contiguous 0..N-1.
    expected = 0
    for tok, idx in sorted_vocab:
        if idx != expected:
            print(f"non-contiguous vocab at id={idx} (expected {expected})",
                  file=sys.stderr)
            return 3
        expected += 1
        if "\n" in tok or "\r" in tok:
            print(f"newline inside token at id={idx}", file=sys.stderr)
            return 4

    with open(out / "vocab.txt", "w", encoding="utf-8", newline="\n") as f:
        for tok, _ in sorted_vocab:
            f.write(tok + "\n")

    with open(out / "merges.txt", "w", encoding="utf-8", newline="\n") as f:
        for m in merges:
            if isinstance(m, list):
                f.write(" ".join(m) + "\n")
            else:
                f.write(m + "\n")

    added = src.get("added_tokens", [])
    special = {
        "cls_id":  -1,
        "sep_id":  -1,
        "pad_id":  -1,
        "unk_id":  -1,
        "mask_id": -1,
    }
    for a in added:
        c = a.get("content")
        if   c == "[CLS]":  special["cls_id"]  = a["id"]
        elif c == "[SEP]":  special["sep_id"]  = a["id"]
        elif c == "[PAD]":  special["pad_id"]  = a["id"]
        elif c == "[UNK]":  special["unk_id"]  = a["id"]
        elif c == "[MASK]": special["mask_id"] = a["id"]

    with open(out / "special_tokens.txt", "w", encoding="utf-8",
              newline="\n") as f:
        for k, v in special.items():
            f.write(f"{k}={v}\n")

    print(f"wrote {len(sorted_vocab)} vocab entries, "
          f"{len(merges)} merges to {out}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    sys.exit(main(sys.argv[1], sys.argv[2]))
