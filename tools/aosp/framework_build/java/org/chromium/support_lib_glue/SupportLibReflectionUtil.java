package org.chromium.support_lib_glue;

import android.webkit.WebSettings;
import java.lang.reflect.InvocationHandler;

/** AndroidX WebKit's discovery entry point for the embedded iframe provider. */
public final class SupportLibReflectionUtil {
    private SupportLibReflectionUtil() {}

    public static InvocationHandler createWebViewProviderFactory() {
        return (proxy, method, args) -> switch (method.getName()) {
            case "getSupportedFeatures" -> new String[] {"ALGORITHMIC_DARKENING"};
            case "getWebkitToCompatConverter" -> (InvocationHandler) (converter, operation, values) -> {
                if (!operation.getName().equals("convertSettings") || !(values[0] instanceof WebSettings)) {
                    throw new UnsupportedOperationException(operation.getName());
                }
                return settings((WebSettings) values[0]);
            };
            default -> throw new UnsupportedOperationException(method.getName());
        };
    }

    private static InvocationHandler settings(WebSettings settings) {
        return (proxy, method, args) -> switch (method.getName()) {
            case "setAlgorithmicDarkeningAllowed" -> {
                settings.setAlgorithmicDarkeningAllowed((Boolean) args[0]);
                yield null;
            }
            case "isAlgorithmicDarkeningAllowed" -> settings.isAlgorithmicDarkeningAllowed();
            default -> throw new UnsupportedOperationException(method.getName());
        };
    }
}
