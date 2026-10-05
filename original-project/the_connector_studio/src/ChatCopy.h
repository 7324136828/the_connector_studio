#pragma once
#include "AppTypes.h"

namespace Lattice::ChatCopy
{
    struct Result { bool success = false; std::wstring text, error; };
    bool HasContent(const MessageItem& message);
    Result Message(const MessageItem& message);
    Result Transcript(const SessionTab& session);
}
