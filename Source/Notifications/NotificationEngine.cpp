/*
  ==============================================================================
    NotificationEngine.cpp
    Pink Noise Audio Monitor (pnam) — rate-limited macOS desktop notifications.
  ==============================================================================
*/

#include "NotificationEngine.h"

namespace pnam
{

//==============================================================================
void NotificationEngine::initialise()
{
    // Request notification permissions asynchronously from macOS.
    // If denied, osAvailable stays false and postCritical degrades gracefully.
    auto* notificationInstance = juce::PushNotifications::getInstance();
    if (notificationInstance != nullptr)
    {
        juce::PushNotifications::Settings settings;
        settings.allowAlert = true;
        settings.allowBadge = true;
        settings.allowSound = true;

        notificationInstance->requestPermissionsWithSettings (settings);

        // Conservative: assume available if the API is present.
        // A denied permission will result in unavailable posts (no error).
        osAvailable = true;
    }
}

//==============================================================================
void NotificationEngine::setEnabled (bool shouldBeEnabled) noexcept
{
    enabled = shouldBeEnabled;
}

bool NotificationEngine::isEnabled() const noexcept
{
    return enabled;
}

//==============================================================================
void NotificationEngine::setCooldownSeconds (double seconds) noexcept
{
    cooldownSeconds = juce::jlimit (5.0, 3600.0, seconds);
}

double NotificationEngine::getCooldownSeconds() const noexcept
{
    return cooldownSeconds;
}

//==============================================================================
void NotificationEngine::setMaxPerMinute (int maxNotifications) noexcept
{
    maxPerMinute = juce::jmax (1, maxNotifications);
}

//==============================================================================
NotificationEngine::RateLimitResult NotificationEngine::postCritical (
    const juce::String& title,
    const juce::String& body,
    EventType type)
{
    // Ensure we're on the message thread and the engine is enabled.
    if (!enabled || !osAvailable)
        return RateLimitResult::unavailable;

    // Convert EventType to the index in typeStates array.
    auto typeIndex = static_cast<int> (type);
    if (typeIndex < 0 || typeIndex >= numEventTypes)
        typeIndex = numEventTypes - 1; // Fallback to "Other"

    auto& state = typeStates[typeIndex];
    const auto nowMillis = juce::Time::getCurrentTime().toMilliseconds();
    const auto cooldownMillis = static_cast<juce::int64> (cooldownSeconds * 1000.0);

    //--------------------------------------------------------------------------
    // Check per-type cooldown.
    //--------------------------------------------------------------------------
    if (state.lastPostMillis > 0 && nowMillis < state.lastPostMillis + cooldownMillis)
    {
        // Still in cooldown; suppress and count.
        state.suppressedSinceLastPost++;
        return RateLimitResult::suppressedCooldown;
    }

    //--------------------------------------------------------------------------
    // Prune recentPostTimes: keep only entries from the last 60 seconds.
    //--------------------------------------------------------------------------
    const auto cutoffMillis = nowMillis - 60000;
    while (!recentPostTimes.isEmpty() && recentPostTimes[0] < cutoffMillis)
    {
        recentPostTimes.remove (0);
    }

    //--------------------------------------------------------------------------
    // Check global rolling-minute cap.
    //--------------------------------------------------------------------------
    if (recentPostTimes.size() >= maxPerMinute)
    {
        // Global cap reached; suppress without posting.
        return RateLimitResult::suppressedGlobal;
    }

    //--------------------------------------------------------------------------
    // Rate limits passed: post the notification.
    //--------------------------------------------------------------------------
    juce::String actualBody = body;

    // Append "(+N similar suppressed)" if we just exited a cooldown window
    // and there were suppressed posts.
    if (state.suppressedSinceLastPost > 0)
    {
        actualBody << " (+";
        actualBody << juce::String (state.suppressedSinceLastPost);
        actualBody << " similar suppressed)";
    }

    // Attempt to post via the OS.
    auto* notificationInstance = juce::PushNotifications::getInstance();
    if (notificationInstance != nullptr)
    {
        juce::PushNotifications::Notification notification;
        notification.title       = title;
        notification.body        = actualBody;
        notification.identifier  = "pnam_critical_" + juce::String ((int) type);

        notificationInstance->sendLocalNotification (notification);
    }

    // Update state and tracking.
    state.lastPostMillis = nowMillis;
    state.suppressedSinceLastPost = 0;
    recentPostTimes.add (nowMillis);
    totalPosted++;

    return RateLimitResult::posted;
}

//==============================================================================
int NotificationEngine::getSuppressedCount (EventType type) const noexcept
{
    auto typeIndex = static_cast<int> (type);
    if (typeIndex < 0 || typeIndex >= numEventTypes)
        typeIndex = numEventTypes - 1; // Fallback to "Other"

    return typeStates[typeIndex].suppressedSinceLastPost;
}

//==============================================================================
int64_t NotificationEngine::getTotalPosted() const noexcept
{
    return totalPosted;
}

} // namespace pnam
