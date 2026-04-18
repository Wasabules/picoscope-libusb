# R8 / ProGuard rules applied to consumers of this library.
# Native methods on PicoScope2204A must be preserved, otherwise the JNI
# lookup via System.loadLibrary will fail with UnsatisfiedLinkError at
# runtime.
-keep class io.github.wasabules.ps2204.PicoScope2204A {
    native <methods>;
    public static final int *;
    public static final java.lang.String *;
}
