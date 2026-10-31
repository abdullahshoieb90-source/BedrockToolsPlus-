#pragma once
#include <cstdarg>
#include <cstdint>
using jint=int; using jlong=long long; using jboolean=bool;
using jobject=void*; using jclass=void*; using jmethodID=void*;
constexpr int JNI_OK=0, JNI_VERSION_1_6=0x10006;
struct JNIEnv {
    bool pending=false, onMain=true, resumed=true, haveActivity=true;
    int action=-1, calls=0, frames=0; jlong downTime=0, eventTime=0;
    float x=0,y=0;
    jint PushLocalFrame(jint) { ++frames; return 0; }
    jobject PopLocalFrame(jobject) { --frames; return nullptr; }
    bool ExceptionCheck() { return pending; }
    void ExceptionClear() { pending=false; }
    jclass FindClass(const char*) { return reinterpret_cast<void*>(1); }
    jmethodID GetStaticMethodID(jclass,const char* name,const char*) {
        if (__builtin_strcmp(name,"myLooper")==0) return (void*)2;
        if (__builtin_strcmp(name,"getMainLooper")==0) return (void*)3;
        if (__builtin_strcmp(name,"getCurrentActivity")==0) return (void*)4;
        if (__builtin_strcmp(name,"isResumed")==0) return (void*)5;
        if (__builtin_strcmp(name,"uptimeMillis")==0) return (void*)6;
        return (void*)7;
    }
    jmethodID GetMethodID(jclass,const char*,const char*) { return (void*)8; }
    jobject CallStaticObjectMethod(jclass,jmethodID m,...) {
        if (m==(void*)2) return reinterpret_cast<void*>(static_cast<std::uintptr_t>(onMain?10:11));
        if (m==(void*)3) return (void*)10;
        if (m==(void*)4) return haveActivity?(void*)12:nullptr;
        va_list args; va_start(args,m); downTime=va_arg(args,jlong); eventTime=va_arg(args,jlong);
        action=va_arg(args,int); x=static_cast<float>(va_arg(args,double)); y=static_cast<float>(va_arg(args,double));
        va_end(args); return (void*)13;
    }
    bool IsSameObject(jobject a,jobject b) {return a==b;}
    jboolean CallStaticBooleanMethod(jclass,jmethodID) {return resumed;}
    jlong CallStaticLongMethod(jclass,jmethodID) {return ++clock;}
    jclass GetObjectClass(jobject) {return (void*)1;}
    void CallVoidMethod(jobject,jmethodID,...) {}
    jboolean CallBooleanMethod(jobject,jmethodID,jobject) {++calls; return true;}
    jlong clock=100;
};
struct JavaVM {
    JNIEnv* env=nullptr; bool attached=true;
    jint GetEnv(void** out,jint) {if(!attached) return -2; *out=env; return JNI_OK;}
};
