import static android.content.pm.PackageManager.INSTALL_FAILED_OLDER_SDK;
import static android.content.pm.PackageManager.INSTALL_PARSE_FAILED_MANIFEST_MALFORMED;
import static android.content.pm.PackageManager.INSTALL_SUCCEEDED;

import android.content.pm.PackageInfo;
import android.content.pm.SharedLibraryInfo;
import android.content.pm.SigningDetails;
import android.content.pm.parsing.ApkLite;
import android.content.pm.parsing.ApkLiteParseUtils;
import android.content.pm.parsing.FrameworkParsingPackageUtils;
import android.content.pm.parsing.PackageLite;
import android.content.pm.parsing.result.ParseInput;
import android.content.pm.parsing.result.ParseResult;
import android.content.pm.parsing.result.ParseTypeImpl;
import android.content.res.ApkAssets;
import android.content.res.XmlResourceParser;
import android.util.Pair;

import com.android.internal.pm.pkg.parsing.ParsingUtils;

import java.io.File;
import java.util.List;
import java.util.Objects;

public final class PmBehaviorTest {
    private static final String PACKAGE = "org.wasmandroid.pm.fixture";
    private static final String SPLIT = "feature.metrics";

    private static void equal(Object actual, Object expected, String field) {
        if (!Objects.equals(actual, expected)) {
            throw new AssertionError(field + " expected " + expected + ", got " + actual);
        }
    }

    private static <T> T success(ParseResult<T> result) {
        equal(result.getErrorCode(), INSTALL_SUCCEEDED, result.getErrorMessage());
        equal(result.isSuccess(), true, "success flag");
        return result.getResult();
    }

    private static void checkNames() {
        equal(FrameworkParsingPackageUtils.validateName(PACKAGE, true, true), null, PACKAGE);
        equal(FrameworkParsingPackageUtils.validateName("com.Example2.app_1", true, true), null,
                "ASCII letters, digits and underscores");
        equal(FrameworkParsingPackageUtils.validateName("single", true, true),
                "must have at least one '.' separator", "missing package separator");
        equal(FrameworkParsingPackageUtils.validateName("single", false, false), null,
                "unqualified split name");
        equal(FrameworkParsingPackageUtils.validateName("org.7example", true, true),
                "bad character '7'", "digit at start of component");
        equal(FrameworkParsingPackageUtils.validateName("org.example-name", true, true),
                "bad character '-'", "hyphen in a package name");
        equal(FrameworkParsingPackageUtils.validateName("org.example/app", true, true),
                "bad character '/'", "path separator");
        equal(FrameworkParsingPackageUtils.validateName("p." + "a".repeat(221), true, true), null,
                "223-character package name");
        equal(FrameworkParsingPackageUtils.validateName("p." + "a".repeat(222), true, true),
                "the length of the name is greater than 223", "224-character package name");
        equal(ParsingUtils.buildClassName(PACKAGE, ".Main"), PACKAGE + ".Main", "relative class");
        equal(ParsingUtils.buildClassName(PACKAGE, "Main"), PACKAGE + ".Main", "bare class");
        equal(ParsingUtils.buildClassName(PACKAGE, "org.other.Main"), "org.other.Main",
                "fully qualified class");
        equal(ParsingUtils.buildClassName(PACKAGE, ""), null, "empty class name");
    }

    private static void checkParseResult(ParseTypeImpl input) {
        success(FrameworkParsingPackageUtils.validateName(input.reset(), PACKAGE, true, true));
        ParseResult<?> failure = FrameworkParsingPackageUtils.validateName(
                input.reset(), "org.7example", true, true);
        equal(failure.isError(), true, "validation error");
        equal(failure.getErrorCode(), INSTALL_PARSE_FAILED_MANIFEST_MALFORMED, "validation code");
        equal(failure.getErrorMessage(), "bad character '7'", "validation message");
        equal(success(input.reset().success("reused")), "reused", "result after reset");
        equal(input.getErrorMessage(), null, "message after reset");

        long change = ParseInput.DeferredError.MISSING_EXPORTED_FLAG;
        success(input.reset().deferError("Missing exported attribute", change));
        success(input.enableDeferredError(PACKAGE, 30));
        success(input.reset().deferError("Missing exported attribute", change));
        failure = input.enableDeferredError(PACKAGE, 31);
        equal(failure.getErrorCode(), INSTALL_PARSE_FAILED_MANIFEST_MALFORMED, "deferred error code");
        equal(failure.getErrorMessage(), "Missing exported attribute", "deferred error message");
    }

    private static void checkSplitNames(ParseTypeImpl input, File file, String split) throws Exception {
        ApkAssets assets = ApkAssets.loadFromPath(file.getAbsolutePath());
        try (XmlResourceParser pull = assets.openXml(ApkLiteParseUtils.ANDROID_MANIFEST_FILENAME)) {
            equal(pull.getClass().getName(), "android.content.res.XmlBlock$Parser", "binary XML parser");
            Pair<String, String> names = success(ApkLiteParseUtils.parsePackageSplitNames(input.reset(), pull));
            equal(names.first, PACKAGE, "manifest package");
            equal(names.second, split, "manifest split");
            equal(pull.getName(), "manifest", "parser position after reading names");
        } finally {
            assets.close();
        }
    }

    private static void checkApkParsing(ParseTypeImpl input, File base, File split, File future) {
        ApkLite apk = success(ApkLiteParseUtils.parseApkLite(input.reset(), base, 0));
        equal(apk.getPackageName(), PACKAGE, "package name");
        equal(apk.getSplitName(), null, "base split name");
        equal(apk.getVersionCode(), 17, "version code");
        equal(apk.getVersionCodeMajor(), 2, "major version code");
        equal(apk.getRevisionCode(), 5, "revision code");
        equal(apk.getInstallLocation(), PackageInfo.INSTALL_LOCATION_INTERNAL_ONLY, "install location");
        equal(apk.getMinSdkVersion(), 23, "min SDK");
        equal(apk.getTargetSdkVersion(), 36, "target SDK");
        equal(apk.isDebuggable(), true, "debuggable");
        equal(apk.isProfileableByShell(), true, "profileable by shell");
        equal(apk.isExtractNativeLibs(), false, "extract native libraries");
        equal(apk.getSigningDetails(), SigningDetails.UNKNOWN, "unsigned parse");
        equal(apk.getDeclaredLibraries().size(), 1, "declared library count");
        SharedLibraryInfo library = apk.getDeclaredLibraries().get(0);
        equal(library.getName(), "org.wasmandroid.fixture.library", "declared library name");
        equal(library.getType(), SharedLibraryInfo.TYPE_DYNAMIC, "declared library type");

        ApkLite feature = success(ApkLiteParseUtils.parseApkLite(input.reset(), split, 0));
        equal(feature.getSplitName(), SPLIT, "feature split name");
        equal(feature.isFeatureSplit(), true, "feature split flag");
        equal(feature.getUsesSplitName(), "feature.shared", "split dependency");
        equal(ApkLiteParseUtils.splitNameToFileName(feature), "split_feature.metrics.apk", "split filename");

        PackageLite pkg = success(ApkLiteParseUtils.parsePackageLite(input.reset(), base, 0));
        equal(pkg.getPackageName(), PACKAGE, "monolithic package name");
        equal(pkg.getAllApkPaths(), List.of(base.getAbsolutePath()), "monolithic APK paths");

        ParseResult<ApkLite> rejected = ApkLiteParseUtils.parseApkLite(input.reset(), future, 0);
        equal(rejected.isError(), true, "future SDK rejection");
        equal(rejected.getErrorCode(), INSTALL_FAILED_OLDER_SDK, "future SDK error code");
        equal(rejected.getErrorMessage(), "Requires newer sdk version #99 (current version is #37)",
                "future SDK error message");
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 4) {
            throw new IllegalArgumentException("Expected JNI library, base, split and future APK paths");
        }
        System.load(args[0]);
        File base = new File(args[1]);
        File split = new File(args[2]);
        checkNames();
        ParseTypeImpl input = ParseTypeImpl.forDefaultParsing();
        checkParseResult(input);
        checkSplitNames(input, base, null);
        checkSplitNames(input, split, SPLIT);
        checkApkParsing(input, base, split, new File(args[3]));
        System.out.println("AOSP 17 JVM name validation, deferred errors and aapt2 base/split APK parsing passed");
    }
}
