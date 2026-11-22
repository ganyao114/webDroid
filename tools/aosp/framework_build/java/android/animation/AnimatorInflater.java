package android.animation;

import android.content.Context;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.view.InflateException;
import android.view.animation.AnimationUtils;
import java.io.IOException;
import java.util.ArrayList;
import org.wasmandroid.ui.XmlValues;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

/** Inflates float property animations from compiled Android resources. */
public final class AnimatorInflater {
    private AnimatorInflater() {}

    public static Animator loadAnimator(Context context, int id) throws Resources.NotFoundException {
        try (XmlResourceParser parser = context.getResources().getAnimation(id)) {
            XmlValues.toStartTag(parser);
            if (!"objectAnimator".equals(parser.getName())) throw new InflateException("Expected objectAnimator");
            XmlValues attrs = new XmlValues(context.getResources(), context.getTheme(), parser);
            String property = attrs.text("propertyName", null);
            int duration = attrs.integer("duration", 300);
            int delay = attrs.integer("startOffset", 0);
            int repeats = attrs.integer("repeatCount", 0);
            int repeatMode = attrs.integer("repeatMode", ValueAnimator.RESTART);
            int interpolator = attrs.id("interpolator", 0);
            float start = attrs.number("valueFrom", 0);
            float end = attrs.number("valueTo", 0);
            ObjectAnimator result = null;
            int depth = parser.getDepth();
            for (int event = parser.next(); event != XmlPullParser.END_DOCUMENT; event = parser.next()) {
                if (event == XmlPullParser.END_TAG && parser.getDepth() == depth) break;
                if (event != XmlPullParser.START_TAG) continue;
                if (!"propertyValuesHolder".equals(parser.getName()) || result != null) {
                    throw new InflateException("Expected one float propertyValuesHolder");
                }
                result = loadFloatKeyframes(context, parser, attrs);
            }
            if (result == null) result = ObjectAnimator.forFloatKeyframes(property,
                    new float[] {0, 1}, new float[] {start, end}, new TimeInterpolator[2]);
            result.setDuration(duration);
            result.setStartDelay(delay);
            result.setRepeatCount(repeats);
            result.setRepeatMode(repeatMode);
            if (interpolator != 0) result.setInterpolator(AnimationUtils.loadInterpolator(context, interpolator));
            return result;
        } catch (IOException | XmlPullParserException error) {
            throw new Resources.NotFoundException("Cannot inflate animator " + id, error);
        }
    }

    private static ObjectAnimator loadFloatKeyframes(Context context, XmlResourceParser parser, XmlValues attrs)
            throws IOException, XmlPullParserException {
        String property = attrs.text("propertyName", null);
        ArrayList<Float> fractions = new ArrayList<>(), values = new ArrayList<>();
        ArrayList<TimeInterpolator> interpolators = new ArrayList<>();
        int depth = parser.getDepth();
        for (int event = parser.next(); event != XmlPullParser.END_DOCUMENT; event = parser.next()) {
            if (event == XmlPullParser.END_TAG && parser.getDepth() == depth) break;
            if (event != XmlPullParser.START_TAG) continue;
            if (!"keyframe".equals(parser.getName())) throw new InflateException("Expected keyframe");
            float fraction = attrs.number("fraction", -1);
            if (fraction < 0 || fraction > 1 || (!fractions.isEmpty() && fraction <= fractions.get(fractions.size() - 1))) {
                throw new InflateException("Expected increasing explicit keyframe fractions");
            }
            fractions.add(fraction);
            values.add(attrs.number("value", 0));
            int interpolator = attrs.id("interpolator", 0);
            interpolators.add(interpolator == 0 ? null : AnimationUtils.loadInterpolator(context, interpolator));
            XmlValues.skip(parser);
        }
        int count = fractions.size();
        if (count < 2 || fractions.get(0) != 0 || fractions.get(count - 1) != 1) {
            throw new InflateException("Expected keyframes at both endpoints");
        }
        float[] times = new float[count], stops = new float[count];
        for (int i = 0; i < count; i++) { times[i] = fractions.get(i); stops[i] = values.get(i); }
        return ObjectAnimator.forFloatKeyframes(property, times, stops, interpolators.toArray(new TimeInterpolator[0]));
    }
}
