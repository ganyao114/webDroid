package android.annotation;

public @interface Size {
    long min() default Long.MIN_VALUE;
    long max() default Long.MAX_VALUE;
    long value() default -1;
    long multiple() default 1;
}
