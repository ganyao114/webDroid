package com.android.internal.pm.pkg.component;

public final class AconfigFlags {
    public static AconfigFlags getInstance() {
        throw new UnsupportedOperationException("Fixture has no aconfig declarations");
    }

    public Boolean getFlagValue(String name) {
        throw new UnsupportedOperationException(name);
    }

    public boolean skip(Object pkg, String name, boolean negated) {
        throw new UnsupportedOperationException(name);
    }
}
