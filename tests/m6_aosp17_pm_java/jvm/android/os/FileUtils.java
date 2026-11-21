package android.os;

import java.nio.charset.StandardCharsets;

// Emulate just Android's external filename constraint for validateName on HotSpot.
public final class FileUtils {
    public static boolean isValidExtFilename(String name) {
        return name != null && !name.isEmpty() && !name.equals(".") && !name.equals("..")
                && name.indexOf('/') < 0 && name.indexOf('\0') < 0
                && name.getBytes(StandardCharsets.UTF_8).length <= 255;
    }
}
