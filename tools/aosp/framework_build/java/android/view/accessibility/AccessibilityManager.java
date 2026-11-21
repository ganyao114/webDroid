package android.view.accessibility;

/** Accessibility service for a window embedded in the host compositor. */
public final class AccessibilityManager {
    public boolean isEnabled() { return false; }
    public boolean isTouchExplorationEnabled() { return false; }
    public int getRecommendedTimeoutMillis(int timeout, int flags) { return timeout; }
    public void sendAccessibilityEvent(AccessibilityEvent event) {}
}
