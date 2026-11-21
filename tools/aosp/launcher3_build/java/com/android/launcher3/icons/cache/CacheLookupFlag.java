package com.android.launcher3.icons.cache;

public final class CacheLookupFlag {
    public static final CacheLookupFlag DEFAULT_LOOKUP_FLAG = new CacheLookupFlag(false, false);
    private final boolean themed;
    private final boolean lowRes;

    private CacheLookupFlag(boolean themed, boolean lowRes) {
        this.themed = themed;
        this.lowRes = lowRes;
    }

    public CacheLookupFlag withThemeIcon() {
        return themed ? this : new CacheLookupFlag(true, lowRes);
    }

    public CacheLookupFlag withLowRes() {
        return lowRes ? this : new CacheLookupFlag(themed, true);
    }

    public boolean useLowRes() {
        return lowRes;
    }
}
