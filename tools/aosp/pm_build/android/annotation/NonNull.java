// Compile-time declaration for an annotation hidden by the public SDK.
package android.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.CLASS)
public @interface NonNull {}
