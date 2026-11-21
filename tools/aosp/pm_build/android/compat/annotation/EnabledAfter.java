// Compile-time declaration for a hidden AOSP API.
package android.compat.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.SOURCE)
public @interface EnabledAfter {
    int targetSdkVersion();
}
