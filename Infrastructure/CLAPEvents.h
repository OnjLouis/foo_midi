
/** $VER: CLAPEventList.h (2025.07.09) P. Stuer **/

#pragma once

#include <CppCoreCheck/Warnings.h>
#pragma warning(disable: 5045 ALL_CPPCORECHECK_WARNINGS)

#include <vector>
#include <memory>

#include <clap/clap.h>

#include "Resource.h"
#include "Encoding.h"
#include "CLAPMidiInputs.h"

namespace CLAP
{

/// <summary>
/// Implements an output event list for the CLAP plug-in.
/// </summary>
struct OutputEvents : clap_output_events
{
public:
    OutputEvents();

public:
    std::vector<std::unique_ptr<clap_event_header>> Events;
};

}
