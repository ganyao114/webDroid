package android.content.pm;

import android.os.Handler;
import java.util.Collections;
import java.util.List;

/** The host installs APKs atomically, so there are no guest installation sessions. */
public final class PackageInstaller {
    public static final int PACKAGE_SOURCE_UNSPECIFIED = 0;
    @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.SOURCE)
    public @interface PackageSourceType {}

    public abstract static class SessionCallback {
        public abstract void onCreated(int sessionId);
        public abstract void onBadgingChanged(int sessionId);
        public abstract void onActiveChanged(int sessionId, boolean active);
        public abstract void onProgressChanged(int sessionId, float progress);
        public abstract void onFinished(int sessionId, boolean success);
    }

    public static class SessionInfo {}

    public SessionInfo getSessionInfo(int sessionId) { return null; }
    public List<SessionInfo> getAllSessions() { return Collections.emptyList(); }
    public void registerSessionCallback(SessionCallback callback, Handler handler) {
        java.util.Objects.requireNonNull(callback);
    }
    public void unregisterSessionCallback(SessionCallback callback) {}
}
