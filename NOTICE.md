# Third-party notices

WARP includes or downloads the following third-party components. Release packages also carry the
upstream license files under `licenses\` (gap analysis SEC-2 / ENG-5).

| Component | Version / revision | Used for | License | Source |
|---|---|---|---|---|
| SQLite (amalgamation, `sqlite3.c`) | bundled source | activity store | Public domain | https://www.sqlite.org/copyright.html |
| ONNX Runtime (`Microsoft.ML.OnnxRuntime`) | 1.23.0 (x64/ARM64), 1.22.0 (x86) | embedding inference | MIT | https://github.com/microsoft/onnxruntime |
| ONNX Runtime GenAI (`Microsoft.ML.OnnxRuntimeGenAI`) | 0.14.1 | optional LLM refiner (off by default) | MIT | https://github.com/microsoft/onnxruntime-genai |
| granite-embedding-small-english-r2 (IBM), ONNX export by onnx-community | pinned in `scripts/models.lock.json` | sentence embeddings | Apache-2.0 | https://huggingface.co/ibm-granite/granite-embedding-small-english-r2 |
| Qwen3-0.6B (Qwen team, Alibaba Cloud), ORT-GenAI export by xiaoyao9184 | pinned in `scripts/models.lock.json` | optional LLM refiner (off by default) | Apache-2.0 | https://huggingface.co/Qwen/Qwen3-0.6B |

Model files are not stored in this repository. The build downloads them from immutable
Hugging Face commit URLs and verifies their SHA-256 hashes (`scripts/fetch_models.ps1`).

## SQLite

The author disclaims copyright to this source code. In place of a legal notice, here is a blessing:

> May you do good and not evil.
> May you find forgiveness for yourself and forgive others.
> May you share freely, never taking more than you give.

## ONNX Runtime and ONNX Runtime GenAI (MIT)

```
MIT License

Copyright (c) Microsoft Corporation.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

ONNX Runtime's own third-party notices ship as `licenses\onnxruntime-ThirdPartyNotices.txt`.

## Models (Apache License 2.0)

granite-embedding-small-english-r2 is Copyright IBM Corp.; Qwen3-0.6B is Copyright Alibaba Cloud
(Qwen team). Both are licensed under the Apache License, Version 2.0
(https://www.apache.org/licenses/LICENSE-2.0); the full text ships as `licenses\Apache-2.0.txt`.
The models are redistributed unmodified except for the ORT-GenAI `genai_config.json` session
option that caps the refiner's thread count.
