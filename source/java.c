#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include "music.h"

/*
 * MainActivity methods used by the BTD5 native library.  These correspond to
 * the pinned APK's Dalvik declarations, not to an invented Android facade.
 * Network, advertising, storefront, clipboard and keyboard operations remain
 * intentionally offline/no-op on Vita.
 */

static jstring btd5_string(const char *value) {
    return jni->NewStringUTF(&jni, value);
}

static jobject get_null_object(jmethodID id, va_list args) {
    (void)id; (void)args;
    return NULL;
}

static jobject load_music(jmethodID id, va_list args) {
    (void)id;
    jstring name = va_arg(args, jstring);
    if (!name) {
        return NULL;
    }
    const char *asset_name = jni->GetStringUTFChars(&jni, name, NULL);
    if (!asset_name) {
        return NULL;
    }
    jobject player = (jobject)btd5_music_load(asset_name);
    jni->ReleaseStringUTFChars(&jni, name, (char *)asset_name);
    return player;
}

static jobject get_activity(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)1;
}

static jstring get_bundle_name(jmethodID id, va_list args) {
    (void)id; (void)args;
    return btd5_string("com.ninjakiwi.bloonstd5");
}

static jstring get_data_path(jmethodID id, va_list args) {
    (void)id; (void)args;
    return btd5_string(DATA_PATH);
}

static jstring get_language_code(jmethodID id, va_list args) {
    (void)id; (void)args;
    return btd5_string("en");
}

static jstring get_country_code(jmethodID id, va_list args) {
    (void)id; (void)args;
    return btd5_string("US");
}

static jstring get_empty_string(jmethodID id, va_list args) {
    (void)id; (void)args;
    return btd5_string("");
}

static jstring get_version_name(jmethodID id, va_list args) {
    (void)id; (void)args;
    return btd5_string("3.37");
}

static jint get_zero(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static jint get_version_number(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 1021874;
}

static jint get_audio_sample_rate(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 22050;
}

static jint get_audio_frames_per_buffer(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 1024;
}

static jint get_value_from_key(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jstring);
    (void)va_arg(args, jstring);
    return va_arg(args, jint);
}

static jlong get_zero_long(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static jdouble get_screen_size_inches(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 5.0;
}

static jboolean get_false(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

static jboolean get_true(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_TRUE;
}

static void noop(jmethodID id, va_list args) {
    (void)id; (void)args;
}

static void play_music(jmethodID id, va_list args) {
    (void)id;
    btd5_music_play(va_arg(args, jobject), true);
}

static void play_music_no_loop(jmethodID id, va_list args) {
    (void)id;
    btd5_music_play(va_arg(args, jobject), false);
}

static void pause_music(jmethodID id, va_list args) {
    (void)id;
    btd5_music_pause(va_arg(args, jobject));
}

static void set_music_volume(jmethodID id, va_list args) {
    (void)id;
    void *player = va_arg(args, jobject);
    float volume = (float)va_arg(args, double);
    btd5_music_set_volume(player, volume);
}

static void unload_music(jmethodID id, va_list args) {
    (void)id;
    btd5_music_unload(va_arg(args, jobject));
}

NameToMethodID nameToMethodId[] = {
    {100, "getActivity", METHOD_TYPE_OBJECT},
    {101, "getAdID", METHOD_TYPE_OBJECT},
    {102, "getBundleName", METHOD_TYPE_OBJECT},
    {103, "getCacheStoragePath", METHOD_TYPE_OBJECT},
    {104, "getCountryCode", METHOD_TYPE_OBJECT},
    {105, "getDeviceID", METHOD_TYPE_OBJECT},
    {106, "getExecutablePath", METHOD_TYPE_OBJECT},
    {107, "getExternalFilesPath", METHOD_TYPE_OBJECT},
    {108, "getExternalStoragePath", METHOD_TYPE_OBJECT},
    {109, "getHttpProxyConfig", METHOD_TYPE_OBJECT},
    {110, "getHttpProxyHostName", METHOD_TYPE_OBJECT},
    {111, "getInternalStoragePath", METHOD_TYPE_OBJECT},
    {112, "getLanguageCode", METHOD_TYPE_OBJECT},
    {113, "getUniqueID", METHOD_TYPE_OBJECT},
    {114, "getVersionName", METHOD_TYPE_OBJECT},
    {115, "pasteFromClipboard", METHOD_TYPE_OBJECT},
    {116, "loadMusic", METHOD_TYPE_OBJECT},
    {200, "getHttpProxyPort", METHOD_TYPE_INT},
    {201, "getMemoryUsage", METHOD_TYPE_INT},
    {202, "getNativeAudioFramesPerBuffer", METHOD_TYPE_INT},
    {203, "getNativeAudioSampleRate", METHOD_TYPE_INT},
    {204, "getNetworkType", METHOD_TYPE_INT},
    {205, "getValueFromKey", METHOD_TYPE_INT},
    {206, "getVersionNumber", METHOD_TYPE_INT},
    {207, "hasClipboardTextEntry", METHOD_TYPE_INT},
    {208, "isOnline", METHOD_TYPE_INT},
    {209, "openURL", METHOD_TYPE_INT},
    {300, "getDeviceBootTime", METHOD_TYPE_LONG},
    {301, "getTotalSystemMemoryInBytes", METHOD_TYPE_LONG},
    {400, "getScreenSizeInches", METHOD_TYPE_DOUBLE},
    {500, "getStoragePermissionGranted", METHOD_TYPE_BOOLEAN},
    {501, "showStorageRequestExplanation", METHOD_TYPE_BOOLEAN},
    {600, "SetKeyboardInputType", METHOD_TYPE_VOID},
    {601, "SetKeyboardMaxCharacters", METHOD_TYPE_VOID},
    {602, "ShowKeyboard", METHOD_TYPE_VOID},
    {603, "addWindowFlags", METHOD_TYPE_VOID},
    {604, "clearWindowFlags", METHOD_TYPE_VOID},
    {605, "copyToClipboard", METHOD_TYPE_VOID},
    {607, "performPermissionRequest", METHOD_TYPE_VOID},
    {608, "quitApplication", METHOD_TYPE_VOID},
    {609, "requestStoragePermission", METHOD_TYPE_VOID},
    {610, "runOnGameThread", METHOD_TYPE_VOID},
    {611, "setAllowNativeKeyDown", METHOD_TYPE_VOID},
    {612, "setInputDisabled", METHOD_TYPE_VOID},
    {613, "setNativeViewVisible", METHOD_TYPE_VOID},
    {614, "setScreenCanTimeout", METHOD_TYPE_VOID},
    {615, "showMessageBox", METHOD_TYPE_VOID},
    {616, "showRatingPrompt", METHOD_TYPE_VOID},
    {617, "startActivityForResult", METHOD_TYPE_VOID},
    {618, "startSendIntent", METHOD_TYPE_VOID},
    {619, "storeKeyValuePair", METHOD_TYPE_VOID},
    {620, "unloadMusic", METHOD_TYPE_VOID},
    {621, "playMusic", METHOD_TYPE_VOID},
    {622, "playMusicNoLoop", METHOD_TYPE_VOID},
    {623, "pauseMusic", METHOD_TYPE_VOID},
    {624, "setVolume", METHOD_TYPE_VOID},
};

MethodsBoolean methodsBoolean[] = {
    {500, get_true},
    {501, get_false},
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {{400, get_screen_size_inches}};
MethodsFloat methodsFloat[] = {};
MethodsInt methodsInt[] = {
    {200, get_zero}, {201, get_zero}, {202, get_audio_frames_per_buffer},
    {203, get_audio_sample_rate}, {204, get_zero}, {205, get_value_from_key},
    {206, get_version_number}, {207, get_zero}, {208, get_zero}, {209, get_zero},
};
MethodsLong methodsLong[] = {{300, get_zero_long}, {301, get_zero_long}};
MethodsObject methodsObject[] = {
    {100, get_activity}, {101, get_empty_string}, {102, get_bundle_name},
    {103, get_data_path}, {104, get_country_code}, {105, get_empty_string},
    {106, get_data_path}, {107, get_data_path}, {108, get_data_path},
    {109, get_null_object}, {110, get_empty_string}, {111, get_data_path},
    {112, get_language_code}, {113, get_empty_string}, {114, get_version_name},
    {115, get_empty_string}, {116, load_music},
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    {600, noop}, {601, noop}, {602, noop}, {603, noop}, {604, noop},
    {605, noop}, {607, noop}, {608, noop}, {609, noop},
    {610, noop}, {611, noop}, {612, noop}, {613, noop}, {614, noop},
    {615, noop}, {616, noop}, {617, noop}, {618, noop}, {619, noop},
    {620, unload_music}, {621, play_music}, {622, play_music_no_loop},
    {623, pause_music}, {624, set_music_volume},
};

char WINDOW_SERVICE[] = "window";
const int SDK_INT = 19;

NameToFieldID nameToFieldId[] = {
    {0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT},
    {1, "SDK_INT", FIELD_TYPE_INT},
};
FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {{1, SDK_INT}};
FieldsObject fieldsObject[] = {{0, WINDOW_SERVICE}};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
