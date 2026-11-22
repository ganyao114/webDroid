# Android 17 UI framework build

`upstream.txt` lists the Java sources compiled directly from the pinned Android 17
checkout. The builder regenerates Java AssetManager native declarations from the
same upstream JNI registration table linked by `aosp17_core_jni`. The small
package-local math classes supply the interpreter operations used by upstream
`TypedValue`, interpolators, `Scroller` and `OverScroller` at compile time.
An `@patch` entry names a unified diff in `patches/`. The builder copies the
affected source into its temporary directory before applying the diff. Source
files and patch files are recorded in the build dependencies.

The remaining Java sources implement the UI and application APIs exercised by the
embedded runtime. Graphics geometry compatibility classes cover the source files
missing from the sparse checkout. The interpreter supplies `Log` and `Slog`.
Classes under `java_base` supply boot library APIs that the frozen interpreter does
not define.

Handler threads run against cooperative queues drained by the frame pump.
Preferences and the embedded database live in the VM process. The package
installer and widget host represent a host with atomic APK publication and no
remote widget providers. Launcher measures against the host viewport and reads
icons and strings directly from installed APK resources. The bitmap canvas
supports paths, strokes, rounded clips, alpha masks, and PNG encoding for the
icon cache. The window canvas publishes text and graphics to SurfaceFlinger.

```sh
tools/aosp/build_framework_dex.py --out-dir build-framework --depfile build-framework/framework.d
tools/aosp/build_framework_dex.py --out-dir build-framework --check
```

The first command compiles with JDK 17 and SDK 36 D8 at min API 26. It emits
`aosp17_framework_core.dex` and `aosp17_framework_classes.jar`, with the latter intended
as the first JAR on downstream `javac` classpaths. The second command checks that
both outputs rebuild byte for byte.
