#ifndef BTD5_JAVA_H
#define BTD5_JAVA_H

#include <falso_jni/FalsoJNI.h>

typedef void (*BTD5NativeLicenseResult)(JNIEnv *, jobject, jint, jint);

void btd5_java_bind_license_result(BTD5NativeLicenseResult callback);

#endif
