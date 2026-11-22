package android.os;

public final class Build {
    private Build() {}
    public static final class VERSION {
        private VERSION() {}
        public static final int SDK_INT = 37;
        public static final String RELEASE = "17";
        public static final String CODENAME = "REL";
        public static final String[] ACTIVE_CODENAMES = {};
    }
    public static final class VERSION_CODES {
        private VERSION_CODES() {}
        public static final int HONEYCOMB = 11, JELLY_BEAN_MR1 = 17, JELLY_BEAN_MR2 = 18, KITKAT = 19;
        public static final int O = 26, P = 28, R = 30;
        public static final int VANILLA_ICE_CREAM = 35, BAKLAVA = 36;
    }
}
