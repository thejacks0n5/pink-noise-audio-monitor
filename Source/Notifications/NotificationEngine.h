#pragma once
/*
  ==============================================================================
    NotificationEngine.h
    Pink Noise Audio Monitor (pnam) — rate-limited macOS desktop notifications.

    MESSAGE THREAD ONLY.

    Purpose: during an unattended overnight run, surface Critical events
    (dropout, device lost, logging failure) as macOS user notifications
    without ever flooding Notification Center.

    Implementation contract (for the .cpp author):
      - Delivery: juce::PushNotifications::getInstance()->sendLocalNotification
        with a juce::PushNotifications::Notification (title, body, identifier).
        Requires JUCE_PUSH_NOTIFICATIONS=1 for the target and the app to
        requestPermissionsWithSettings at startup (initialise() does this).
        If permissions are denied or the API is unavailable, notifications
        degrade silently to log-window-only (postCritical still returns a
        meaningful RateLimitResult; the failure is not an error).
      - RATE-LIMIT STRATEGY (fixed contract):
          * One independent cooldown PER EventType, default 60 s
            (setCooldownSeconds). The first notification for a type posts
            immediately; subsequent ones of the SAME type inside the window
            are suppressed and counted.
          * When a suppressed type's window expires, the next post for that
            type appends "(+N similar suppressed)" to its body.
          * Additionally a global cap: at most maxPerMinute (default 6)
            notifications across all types per rolling 60 s.
        This keeps the guarantee: an event storm produces at most ~1 banner
        per type per minute, never hundreds.
      - Only Severity::Critical events should reach postCritical; the caller
        (the UI timer's event drain) enforces that.
  ==============================================================================
*/

#include <cstdint>
#include <juce_core/juce_core.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "../Model/DataModel.h"

namespace pnam
{

//==============================================================================
class NotificationEngine
{
public:
    /** Outcome of a postCritical call, for the live log window. */
    enum class RateLimitResult : int
    {
        posted,             ///< Notification handed to the OS.
        suppressedCooldown, ///< Same-type cooldown active; counted, not shown.
        suppressedGlobal,   ///< Global per-minute cap reached; counted, not shown.
        unavailable         ///< OS notifications unavailable/denied; log-only.
    };

    NotificationEngine() = default;

    NotificationEngine (const NotificationEngine&) = delete;
    NotificationEngine& operator= (const NotificationEngine&) = delete;

    //==========================================================================
    /** Call once at app startup (message thread). Requests notification
        permission from macOS (asynchronous; a denied permission simply makes
        later posts return `unavailable`). */
    void initialise();

    /** Master enable (UI toggle "desktop notifications"). Default true. */
    void setEnabled (bool shouldBeEnabled) noexcept;
    bool isEnabled() const noexcept;

    /** Per-type cooldown window, seconds. Default 60. Clamped to [5, 3600]. */
    void setCooldownSeconds (double seconds) noexcept;
    double getCooldownSeconds() const noexcept;

    /** Global cap across all types per rolling minute. Default 6. */
    void setMaxPerMinute (int maxNotifications) noexcept;

    //==========================================================================
    /** Posts (or rate-limits) a critical desktop notification.
        @param title  short title, e.g. "Audio dropout detected".
        @param body   detail line, typically SessionLogger::composeDescription().
        @param type   used as the rate-limit bucket key.
        @returns what happened, so the caller can annotate the live log. */
    RateLimitResult postCritical (const juce::String& title,
                                  const juce::String& body,
                                  EventType type);

    //==========================================================================
    /** Notifications suppressed (both mechanisms) for a type since the last
        one actually posted for that type. */
    int getSuppressedCount (EventType type) const noexcept;

    /** Total notifications actually posted this app run. */
    int64_t getTotalPosted() const noexcept;

private:
    //==========================================================================
    static constexpr int numEventTypes = 7;   // keep in sync with EventType

    struct TypeState
    {
        juce::int64 lastPostMillis   { 0 };   ///< 0 = never posted.
        int         suppressedSinceLastPost { 0 };
    };

    TypeState   typeStates[numEventTypes];
    bool        enabled            { true };
    bool        osAvailable        { false };
    double      cooldownSeconds    { 60.0 };
    int         maxPerMinute       { 6 };
    int64_t     totalPosted        { 0 };
    juce::Array<juce::int64> recentPostTimes;  ///< rolling-minute window (message thread only).
};

} // namespace pnam
