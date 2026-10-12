// Link stand-ins for the parts of ContextInference that ApiServer references, so the unit tests
// do not need ONNX Runtime or the embedding model. The API tests run with Deps::context == nullptr
// (the context ops then answer "unavailable"); these bodies only satisfy the linker.
#include "ContextInference.h"

std::string ContextInference::GetRecentContext(const std::string&, int64_t, bool)
{
    return "{\"stub\":true}";
}

std::string ContextInference::GetRecentContexts(int, const std::string&, int64_t, bool)
{
    return "{\"stub\":true,\"contexts\":[]}";
}

std::string ContextInference::GetCurrentContextJson(bool)
{
    return "{\"stub\":true}";
}

void ContextInference::ClearHistory() {}
