// Compile-time declaration for a hidden AOSP API.
package com.android.internal.util;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.SOURCE)
public @interface DataClass {
    boolean genAidl() default false;
    boolean genBuilder() default false;
    boolean genConstDefs() default false;
    boolean genConstructor() default false;
    boolean genGetters() default false;
    boolean genParcelable() default false;
    boolean genSetters() default false;

    @Retention(RetentionPolicy.CLASS)
    @interface Generated {
        long time();
        String codegenVersion();
        String sourceFile();
        String inputSignatures();

        @Retention(RetentionPolicy.CLASS)
        @interface Member {}
    }

    @Retention(RetentionPolicy.SOURCE)
    @interface Suppress {
        String[] value();
    }
}
