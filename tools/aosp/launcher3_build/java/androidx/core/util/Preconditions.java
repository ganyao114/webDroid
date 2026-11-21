package androidx.core.util;

import java.util.Objects;

public final class Preconditions {
    private Preconditions() {}

    public static <T> T checkNotNull(T value) { return Objects.requireNonNull(value); }
    public static <T> T checkNotNull(T value, Object message) {
        return Objects.requireNonNull(value, String.valueOf(message));
    }
}
