package com.android.internal;

public final class R {
    private R() {}
    public static final class styleable {
        private styleable() {}
        public static final int[] DecelerateInterpolator = {android.R.attr.factor};
        public static final int DecelerateInterpolator_factor = 0;
        public static final int[] AccelerateInterpolator = DecelerateInterpolator;
        public static final int AccelerateInterpolator_factor = 0;
        public static final int[] OvershootInterpolator = {android.R.attr.tension};
        public static final int OvershootInterpolator_tension = 0;
        public static final int[] PathInterpolator = {android.R.attr.controlX1, android.R.attr.controlY1,
                android.R.attr.controlX2, android.R.attr.controlY2, android.R.attr.pathData};
        public static final int PathInterpolator_controlX1 = 0, PathInterpolator_controlY1 = 1,
                PathInterpolator_controlX2 = 2, PathInterpolator_controlY2 = 3, PathInterpolator_pathData = 4;
    }
}
