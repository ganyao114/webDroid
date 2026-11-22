package android.os.ext;

import java.util.Collections;
import java.util.Map;

/** SDK extension levels for a runtime without updatable system modules. */
public final class SdkExtensions {
    private SdkExtensions() {}

    public static int getExtensionVersion(int extension) {
        if (extension < 30) throw new IllegalArgumentException("SDK extension must be at least 30");
        return 0;
    }
    public static Map<Integer, Integer> getAllExtensionVersions() { return Collections.emptyMap(); }
}
