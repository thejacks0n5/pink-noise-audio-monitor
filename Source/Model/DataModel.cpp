/*
  ==============================================================================
    DataModel.cpp
    Pink Noise Audio Monitor (pnam) — message-thread helpers for DataModel.h.

    Implements the toString() overloads declared (but not defined) in
    DataModel.h. These are used by SessionLogger (CSV columns) and
    EventLogComponent (table cells) — MESSAGE THREAD ONLY.
  ==============================================================================
*/

#include "DataModel.h"

namespace pnam
{

const char* toString (EventType type) noexcept
{
    switch (type)
    {
        case EventType::CompleteDropout: return "CompleteDropout";
        case EventType::Clipping:        return "Clipping";
        case EventType::LevelChange:     return "LevelChange";
        case EventType::ManualMarker:    return "ManualMarker";
        case EventType::AudioDeviceLost: return "AudioDeviceLost";
        case EventType::LoggingError:    return "LoggingError";
        case EventType::AudioSettingsChanged: return "AudioSettingsChanged";
        case EventType::Other:
        default:                         return "Other";
    }
}

const char* toString (Severity severity) noexcept
{
    switch (severity)
    {
        case Severity::Info:     return "Info";
        case Severity::Warning:  return "Warning";
        case Severity::Critical: return "Critical";
        default:                 return "Info";
    }
}

const char* toString (ChannelId channel) noexcept
{
    switch (channel)
    {
        case ChannelId::Left:  return "L";
        case ChannelId::Right: return "R";
        case ChannelId::Both:  return "Both";
        case ChannelId::None:
        default:               return "-";
    }
}

} // namespace pnam
