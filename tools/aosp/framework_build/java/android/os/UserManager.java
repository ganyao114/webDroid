package android.os;

import java.util.List;

/** User queries for the single local profile of the embedded runtime. */
public final class UserManager {
    public boolean isDemoUser() { return false; }
    public boolean isUserUnlocked() { return true; }
    public boolean isUserUnlocked(UserHandle user) { return UserHandle.SYSTEM.equals(user); }
    public boolean isQuietModeEnabled(UserHandle user) { return false; }
    public List<UserHandle> getUserProfiles() { return List.of(UserHandle.SYSTEM); }
    public long getSerialNumberForUser(UserHandle user) { return UserHandle.SYSTEM.equals(user) ? 0 : -1; }
    public UserHandle getUserForSerialNumber(long serial) { return serial == 0 ? UserHandle.SYSTEM : null; }
    public boolean hasUserRestriction(String restriction) { return false; }
}
