#pragma once

#include "../Players/ExternalMIDI/Timeline.h"
#include <memory>

namespace ExternalMIDI
{
    void Register(std::shared_ptr<const Sequence> Song);
    bool Configure(HWND Parent, std::wstring & Primary, std::wstring & Secondary, bool & SplitGS);
}
