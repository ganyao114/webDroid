# Upstream framework JNI audit

The UI smoke test reads native declarations from the loaded
`aosp17_upstream_framework.dex` and checks the linked VM methods. The audit covers
`android.graphics` and its subpackages, `AndroidCharacter`, `Hyphenator`,
`MotionEvent`, `KeyEvent`, and `com.android.internal.util`.

| Release DEX declarations | Unbound at `9980580` | Unbound after this change |
| --- | --- | --- |
| 578 | 139 | 0 |

`Region.nativeSetRegion` returns `void` and the DEX declares a single
`Region.nativeConstructor()` overload. `BaseCanvas.nDrawLines`,
`BaseCanvas.nDrawPoints`, `MotionEvent.nativeInitialize`, both key-code string
methods, and the bitmap finalizer, noop, ashmem query, and alpha extraction had
matching bindings before this change.

The boot runtime also supplies `RegionIterator` when the framework DEX omits it.
It binds the iterator's three native methods when the Java class is present.
The NinePatch finalizer registration likewise accepts framework variants that
declare it.

## Declarations newly bound

### `android.graphics.BaseCanvas`

```text
nDrawBitmapMesh(JJII[FI[IIJ)V
nDrawDoubleRoundRect(JFFFFFFFFFFFFJ)V
nDrawDoubleRoundRect(JFFFF[FFFFF[FJ)V
nDrawGlyphs(J[I[FIIIJJ)V
nDrawMesh(JJIJ)V
nDrawRegion(JJJ)V
nDrawTextOnPath(JLjava/lang/String;JFFIJ)V
nDrawTextOnPath(J[CIIJFFIJ)V
nDrawVertices(JII[FI[FI[II[SIIJ)V
nPunchHole(JFFFFFFF)V
```

### `android.graphics.Bitmap`

```text
nativeComputeColorSpace(J)Landroid/graphics/ColorSpace;
nativeCopyAshmem(J)Landroid/graphics/Bitmap;
nativeCopyAshmemConfig(JI)Landroid/graphics/Bitmap;
nativeCopyPixelsFromBuffer(JLjava/nio/Buffer;)V
nativeCopyPixelsToBuffer(JLjava/nio/Buffer;)V
nativeCreateEmptyAshmemBitmap(IIIJ)Landroid/graphics/Bitmap;
nativeCreateFromParcel(Landroid/os/Parcel;)Landroid/graphics/Bitmap;
nativeExtractGainmap(J)Landroid/graphics/Gainmap;
nativeGetAshmemFD(J)I
nativeGetHardwareBuffer(J)Landroid/hardware/HardwareBuffer;
nativeSetColorSpace(JJ)V
nativeWrapHardwareBufferBitmap(Landroid/hardware/HardwareBuffer;J)Landroid/graphics/Bitmap;
nativeWriteToParcel(JILandroid/os/Parcel;)Z
```

### `android.graphics.Shader`

```text
nativeGetFinalizer()J
```

### `android.graphics.BitmapShader`

```text
nativeCreate(JJIIIZZJ)J
```

### `android.graphics.Canvas`

```text
nClipShader(JJI)V
nRestoreUnclippedLayer(JIJ)V
nSaveUnclippedLayer(JIIII)I
nSetDrawFilter(JJ)V
```

### `android.graphics.CanvasProperty`

```text
nCreateFloat(F)J
nCreatePaint(J)J
```

### `android.graphics.ColorSpace$Rgb$Native`

```text
nativeCreate(FFFFFFF[F)J
nativeGetNativeFinalizer()J
```

### `android.graphics.ComposeShader`

```text
nativeCreate(JJJI)J
```

### `android.graphics.DrawFilter`

```text
nativeDestructor(J)V
```

### `android.graphics.FontFamily`

```text
nAddAxisValue(JIF)V
nAddFont(JLjava/nio/ByteBuffer;III)Z
nAddFontWeightStyle(JLjava/nio/ByteBuffer;III)Z
nCreateFamily(J)J
nGetBuilderReleaseFunc()J
nGetFamilyReleaseFunc()J
nInitBuilder(Ljava/lang/String;I)J
```

### `android.graphics.Interpolator`

```text
nativeConstructor(II)J
nativeDestructor(J)V
nativeReset(JII)V
nativeSetKeyFrame(JII[F[F)V
nativeSetRepeatMirror(JFZ)V
nativeTimeToValues(JI[F)I
```

### `android.graphics.Paint`

```text
nGetTextPath(JILjava/lang/String;IIFFJ)V
nGetTextPath(JI[CIIFFJ)V
nSetXfermode(JJ)V
```

### `android.graphics.PathEffect`

```text
nativeDestructor(J)V
```

### `android.graphics.Region`

```text
nativeConstructor()J
nativeCreateFromParcel(Landroid/os/Parcel;)J
nativeDestructor(J)V
nativeEquals(JJ)Z
nativeGetBoundaryPath(JJ)Z
nativeGetBounds(JLandroid/graphics/Rect;)Z
nativeOp(JIIIII)Z
nativeOp(JJJI)Z
nativeOp(JLandroid/graphics/Rect;JI)Z
nativeSetPath(JJJ)Z
nativeSetRect(JIIII)Z
nativeSetRegion(JJ)V
nativeToString(J)Ljava/lang/String;
nativeWriteToParcel(JLandroid/os/Parcel;)Z
contains(II)Z
isComplex()Z
isEmpty()Z
isRect()Z
quickContains(IIII)Z
quickReject(IIII)Z
quickReject(Landroid/graphics/Region;)Z
scale(FLandroid/graphics/Region;)V
translate(IILandroid/graphics/Region;)V
```

### `android.graphics.Typeface`

```text
nativeAddFontCollections(J)V
nativeForceSetStaticFinalField(Ljava/lang/String;Landroid/graphics/Typeface;)V
nativeReadTypefaces(Ljava/nio/ByteBuffer;I)[J
nativeRegisterLocaleList(Ljava/lang/String;)V
nativeSetFontRenderingBackend(I)Z
nativeWarmUpCache(Ljava/lang/String;)V
nativeWriteTypefaces(Ljava/nio/ByteBuffer;I[J)I
```

### `android.graphics.animation.NativeInterpolatorFactory`

```text
createAccelerateDecelerateInterpolator()J
createAccelerateInterpolator(F)J
createAnticipateInterpolator(F)J
createAnticipateOvershootInterpolator(F)J
createBounceInterpolator()J
createCycleInterpolator(F)J
createDecelerateInterpolator(F)J
createLinearInterpolator()J
createLutInterpolator([F)J
createOvershootInterpolator(F)J
createPathInterpolator([F[F)J
```

### `android.graphics.drawable.AnimatedImageDrawable`

```text
nCreate(JLandroid/graphics/ImageDecoder;IIJZLandroid/graphics/Rect;)J
nDraw(JJ)J
nGetAlpha(J)I
nGetFilterBitmap(J)Z
nGetNativeFinalizer()J
nGetRepeatCount(J)I
nIsRunning(J)Z
nNativeByteSize(J)J
nSetAlpha(JI)V
nSetBounds(JLandroid/graphics/Rect;)V
nSetColorFilter(JJ)V
nSetFilterBitmap(JZ)Z
nSetMirrored(JZ)V
nSetOnAnimationEndListener(JLjava/lang/ref/WeakReference;)V
nSetOnFrameRateHintListener(JLjava/lang/ref/WeakReference;)V
nSetRepeatCount(JI)V
nStart(J)Z
nStop(J)Z
```

### `android.graphics.text.PositionedGlyphs`

```text
nGetAscent(J)F
nGetDescent(J)F
nGetFakeBold(JI)Z
nGetFakeItalic(JI)Z
nGetFont(JI)J
nGetFontCount(J)I
nGetFontId(JI)I
nGetFontRef(JI)J
nGetGlyphCount(J)I
nGetGlyphId(JI)I
nGetItalicOverride(JI)F
nGetTotalAdvance(J)F
nGetWeightOverride(JI)F
nGetX(JI)F
nGetY(JI)F
nReleaseFunc()J
```

### `android.graphics.text.TextRunShaper`

```text
nativeShapeTextRun(Ljava/lang/String;IIIIZJ)J
nativeShapeTextRun([CIIIIZJ)J
```

### `android.text.AndroidCharacter`

```text
getDirectionalities([C[BI)V
getEastAsianWidth(C)I
getEastAsianWidths([CII[B)V
getMirror(C)C
mirror([CII)Z
```

### `android.text.Hyphenator`

```text
nInit()V
```

### `com.android.internal.util.ArrayUtils`

```text
zeroize([B)V
zeroize([C)V
```

### `com.android.internal.util.VirtualRefBasePtr`

```text
nDecStrong(J)V
nIncStrong(J)V
```
