package com.android.internal.annotations;

public @interface VisibleForTesting {
    enum Visibility { PRIVATE, PACKAGE, PROTECTED, NONE }
    Visibility visibility() default Visibility.PRIVATE;
}
