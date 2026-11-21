// Compile-time declaration for a hidden AOSP API.
package android.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.SOURCE)
public @interface SystemApi {
    Client client() default Client.PRIVILEGED_APPS;

    enum Client { PRIVILEGED_APPS, MODULE_LIBRARIES }
}
