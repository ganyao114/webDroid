package android.view.animation;

import android.content.Context;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.os.SystemClock;
import android.view.InflateException;
import java.io.IOException;
import org.wasmandroid.ui.XmlValues;
import org.xmlpull.v1.XmlPullParserException;

public final class AnimationUtils {
    private AnimationUtils() {}
    public static long currentAnimationTimeMillis() { return SystemClock.uptimeMillis(); }
    public static Interpolator loadInterpolator(Context context, int id) {
        try (XmlResourceParser parser = context.getResources().getAnimation(id)) {
            XmlValues.toStartTag(parser);
            switch (parser.getName()) {
                case "pathInterpolator": return new PathInterpolator(context, parser);
                case "linearInterpolator": return new LinearInterpolator();
                case "accelerateInterpolator": return new AccelerateInterpolator(context, parser);
                case "decelerateInterpolator": return new DecelerateInterpolator(context, parser);
                case "accelerateDecelerateInterpolator": return new AccelerateDecelerateInterpolator();
                default: throw new InflateException("Unsupported interpolator " + parser.getName());
            }
        } catch (IOException | XmlPullParserException error) {
            throw new Resources.NotFoundException("Cannot inflate interpolator " + id, error);
        }
    }
}
