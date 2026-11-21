package android.util;

import android.graphics.Path;

public final class PathParser {
    private PathParser() {}
    public static Path createPathFromPathData(String pathData) {
        if (pathData == null) throw new IllegalArgumentException("Path data is null");
        return Path.fromPathData(pathData);
    }
}
