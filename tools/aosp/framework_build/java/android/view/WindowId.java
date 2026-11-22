package android.view;

import android.os.IBinder;

/** Identity for a window attached to the embedded view root. */
public final class WindowId {
    private final IBinder token;
    public WindowId(IBinder token) { this.token = java.util.Objects.requireNonNull(token); }
    @Override public boolean equals(Object other) { return other instanceof WindowId id && id.token == token; }
    @Override public int hashCode() { return token.hashCode(); }
}
