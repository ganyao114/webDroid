package androidx.appfunctions;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/** Compile-time annotation for the omitted AndroidX AppFunctions processor. */
@Retention(RetentionPolicy.SOURCE)
public @interface AppFunctionSerializable {
    boolean isDescribedByKDoc();
}
