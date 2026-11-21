// Compile-time declaration for a hidden AOSP API.
package com.android.internal.util;

import java.lang.annotation.Annotation;

public final class AnnotationValidations {
    public static native void validate(
            Class<? extends Annotation> annotation, Annotation ignored, Object value);
}
