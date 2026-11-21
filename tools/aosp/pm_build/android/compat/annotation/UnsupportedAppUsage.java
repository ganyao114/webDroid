// Compile-time declaration for a hidden AOSP API.
package android.compat.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.CLASS)
public @interface UnsupportedAppUsage {
    int maxTargetSdk() default Integer.MAX_VALUE;
    long trackingBug() default 0;
}
