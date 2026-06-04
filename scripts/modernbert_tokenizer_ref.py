"""
Pure-Python reference implementation of the GPT-2 / ModernBERT byte-level BPE
tokenizer.  Used to validate the WARP C++ ModernBertTokenizer produces the
exact same token IDs as the Hugging Face tokenizers library.

Loads the artefacts produced by `extract_modernbert_tokenizer.py`:
  vocab.txt   one byte-level-encoded token per line, line N = ID N
  merges.txt  one "a b" pair per line, in HF rank order
  special_tokens.txt   key=id pairs for [CLS] / [SEP] / [PAD] / [UNK] / [MASK]
"""
from __future__ import annotations
import sys
import unicodedata
from pathlib import Path


# GPT-2 / RoBERTa pre-tokenizer regex.  Matches:
#   - English contractions: 's, 't, 're, 've, 'm, 'll, 'd
#   - Optional leading space + 1+ Unicode letters
#   - Optional leading space + 1+ Unicode digits
#   - Optional leading space + 1+ non-letter-non-digit-non-space
#   - Runs of whitespace not at end-of-string
#   - Runs of whitespace
import regex as _re

GPT2_RE = _re.compile(
    r"'(?:[sdmt]|ll|ve|re)| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+"
)


def bytes_to_unicode() -> dict[int, str]:
    """Standard GPT-2 256-entry byte -> visible Unicode codepoint map."""
    bs = (
        list(range(ord("!"), ord("~") + 1))
        + list(range(ord("¡"), ord("¬") + 1))
        + list(range(ord("®"), ord("ÿ") + 1))
    )
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return {b: chr(c) for b, c in zip(bs, cs)}


B2U = bytes_to_unicode()


def byte_level_encode(text: str) -> str:
    """Encode UTF-8 bytes of `text` as visible-char string per GPT-2 mapping."""
    return "".join(B2U[b] for b in text.encode("utf-8"))


class ModernBertTokenizer:
    def __init__(self, models_dir: str):
        d = Path(models_dir)
        with open(d / "vocab.txt", encoding="utf-8") as f:
            self.vocab = {line.rstrip("\n"): i
                          for i, line in enumerate(f)}
        self.inv_vocab = {i: t for t, i in self.vocab.items()}
        with open(d / "merges.txt", encoding="utf-8") as f:
            self.merge_rank = {tuple(line.rstrip("\n").split(" ", 1)): i
                               for i, line in enumerate(f) if line.strip()}

        specials = {}
        with open(d / "special_tokens.txt", encoding="utf-8") as f:
            for line in f:
                k, _, v = line.rstrip("\n").partition("=")
                specials[k.strip()] = int(v)
        self.cls_id  = specials["cls_id"]
        self.sep_id  = specials["sep_id"]
        self.pad_id  = specials["pad_id"]
        self.unk_id  = specials["unk_id"]
        self.mask_id = specials["mask_id"]

    def _bpe(self, token: str) -> list[int]:
        """Apply BPE to a single byte-level-encoded pre-token."""
        if token in self.vocab:
            # ignore_merges path: prefer the exact vocab entry when present
            return [self.vocab[token]]

        parts = list(token)
        if len(parts) < 2:
            return [self.vocab.get(token, self.unk_id)]

        while True:
            best_rank = None
            best_idx = -1
            for i in range(len(parts) - 1):
                rank = self.merge_rank.get((parts[i], parts[i + 1]))
                if rank is not None and (best_rank is None or rank < best_rank):
                    best_rank = rank
                    best_idx = i
            if best_idx < 0:
                break
            parts = (parts[:best_idx]
                     + [parts[best_idx] + parts[best_idx + 1]]
                     + parts[best_idx + 2:])

        return [self.vocab.get(p, self.unk_id) for p in parts]

    def encode(self, text: str, max_len: int = 128,
               add_special: bool = True) -> list[int]:
        text = unicodedata.normalize("NFC", text)
        ids: list[int] = []
        if add_special:
            ids.append(self.cls_id)
        for m in GPT2_RE.findall(text):
            encoded = byte_level_encode(m)
            ids.extend(self._bpe(encoded))
            if len(ids) >= max_len - (1 if add_special else 0):
                break
        if add_special:
            ids.append(self.sep_id)
        return ids[:max_len]


def _self_test():
    """Compare our output to Hugging Face's reference tokenizer."""
    from transformers import AutoTokenizer
    hf = AutoTokenizer.from_pretrained(
        "ibm-granite/granite-embedding-small-english-r2")

    import tempfile
    import subprocess
    tmp = Path(tempfile.gettempdir()) / "granite-out"
    if not (tmp / "vocab.txt").exists():
        src_json = Path(tempfile.gettempdir()) / "granite-tokenizer.json"
        subprocess.run([sys.executable,
                        str(Path(__file__).parent
                            / "extract_modernbert_tokenizer.py"),
                        str(src_json), str(tmp)], check=True)

    ours = ModernBertTokenizer(str(tmp))

    tests = [
        "auth.cpp - Visual Studio",
        "PR #123: auth refactor",
        "WhatsApp Web - Chrome",
        "useEffect hook in React",
        "OnnxRuntimeGenAI 0.7.0 -> 0.14.1",
        "C:/Users/me/proj/node_modules",
        "Inbox - Outlook",
        "Daily Standup - Microsoft Teams",
        "Don't break my heart 'cause I won't break yours",
        "Working on auth.cpp at 2pm",
        "naïve façade café",
    ]
    fail = 0
    for t in tests:
        ref = hf.encode(t, add_special_tokens=True)
        got = ours.encode(t, max_len=512)
        ok = ref == got
        if not ok:
            fail += 1
        print(("OK  " if ok else "FAIL") + f"  {t!r}")
        if not ok:
            print(f"      ref: {ref}")
            print(f"      got: {got}")
    print(f"\n{len(tests) - fail}/{len(tests)} matched")
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(_self_test())
