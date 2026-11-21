// Compile-time declaration for a hidden AOSP API.
package com.android.internal.annotations;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.SOURCE)
public @interface VisibleForTesting {
    Visibility visibility() default Visibility.PRIVATE;

    enum Visibility { PRIVATE, PACKAGE }
}
