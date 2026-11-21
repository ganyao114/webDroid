package android.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.SOURCE)
public @interface IntDef {
    long[] value() default {};
    boolean flag() default false;
    String[] prefix() default {};
    String[] suffix() default {};
}
