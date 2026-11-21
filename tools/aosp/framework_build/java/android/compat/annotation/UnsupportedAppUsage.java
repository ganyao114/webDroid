package android.compat.annotation;

public @interface UnsupportedAppUsage {
    int maxTargetSdk() default Integer.MAX_VALUE;
    long trackingBug() default 0;
    String publicAlternatives() default "";
}
