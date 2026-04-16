/*
 * Copyright (c) 2026, the Jeandle-JDK Authors. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

/*
 * Native JVMTI agent for TestFrameComplete.
 *
 * Replicates what async-profiler does: sends SIGPROF to a thread running
 * Jeandle-compiled code, then calls AsyncGetCallTrace from the signal handler
 * with the interrupted thread's ucontext. Counts how many samples return
 * ticks_unknown_Java (-5), which indicates safe_for_sender() failed because
 * Frame_Complete was not set correctly.
 */

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "jni.h"
#include "jvmti.h"

static jvmtiEnv* jvmti = NULL;

// AsyncGetCallTrace data structures (copied from HotSpot forte.cpp).
typedef struct {
    jint lineno;
    jmethodID method_id;
} ASGCT_CallFrame;

typedef struct {
    JNIEnv* env_id;
    jint num_frames;
    ASGCT_CallFrame* frames;
} ASGCT_CallTrace;

typedef void (*ASGCTType)(ASGCT_CallTrace*, jint, void*);

static ASGCTType asgct = NULL;

// Target thread info, set by registerThread().
static JNIEnv* target_env = NULL;
static pthread_t target_thread;

// Sample counters, updated atomically from signal handler.
static volatile int total_samples = 0;
static volatile int success_samples = 0;
static volatile int unknown_java_samples = 0;
static volatile int other_error_samples = 0;

#define MAX_DEPTH 64
#define TICKS_UNKNOWN_JAVA (-5)

// SIGPROF handler — runs on the target thread's context, just like async-profiler.
static void sigprof_handler(int sig, siginfo_t* info, void* ucontext) {
    if (asgct == NULL || target_env == NULL) return;

    ASGCT_CallFrame frames[MAX_DEPTH];
    ASGCT_CallTrace trace;
    trace.frames = frames;
    trace.env_id = target_env;
    trace.num_frames = 0;

    asgct(&trace, MAX_DEPTH, ucontext);

    __sync_fetch_and_add(&total_samples, 1);
    if (trace.num_frames > 0) {
        __sync_fetch_and_add(&success_samples, 1);
    } else if (trace.num_frames == TICKS_UNKNOWN_JAVA) {
        __sync_fetch_and_add(&unknown_java_samples, 1);
    } else {
        __sync_fetch_and_add(&other_error_samples, 1);
    }
}

// Prime jmethodIDs so AsyncGetCallTrace can resolve them.
static void GetJMethodIDs(jclass klass) {
    jint method_count = 0;
    jmethodID* methods = NULL;
    jvmtiError err = jvmti->GetClassMethods(klass, &method_count, &methods);
    if (err == JVMTI_ERROR_NONE && methods != NULL) {
        jvmti->Deallocate((unsigned char*)methods);
    }
}

// AsyncGetCallTrace requires class load events to be enabled.
static void JNICALL OnClassLoad(jvmtiEnv* jvmti, JNIEnv* jni_env,
                                jthread thread, jclass klass) {
}

static void JNICALL OnClassPrepare(jvmtiEnv* jvmti, JNIEnv* jni_env,
                                   jthread thread, jclass klass) {
    GetJMethodIDs(klass);
}

static void JNICALL OnVMInit(jvmtiEnv* jvmti, JNIEnv* jni_env, jthread thread) {
    jint class_count = 0;
    jclass* classes = NULL;
    jvmtiError err = jvmti->GetLoadedClasses(&class_count, &classes);
    if (err != JVMTI_ERROR_NONE) return;
    for (int i = 0; i < class_count; i++) {
        GetJMethodIDs(classes[i]);
    }
    jvmti->Deallocate((unsigned char*)classes);
}

extern "C" {

JNIEXPORT
jint JNICALL Agent_OnLoad(JavaVM* jvm, char* options, void* reserved) {
    jint res = jvm->GetEnv((void**)&jvmti, JVMTI_VERSION);
    if (res != JNI_OK || jvmti == NULL) {
        fprintf(stderr, "Error: GetEnv failed\n");
        return JNI_ERR;
    }

    asgct = (ASGCTType)dlsym(RTLD_DEFAULT, "AsyncGetCallTrace");
    if (asgct == NULL) {
        fprintf(stderr, "Error: AsyncGetCallTrace not found\n");
        return JNI_ERR;
    }

    jvmtiCapabilities caps;
    memset(&caps, 0, sizeof(caps));
    caps.can_get_line_numbers = 1;
    caps.can_get_source_file_name = 1;
    jvmti->AddCapabilities(&caps);

    jvmtiEventCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.VMInit = &OnVMInit;
    callbacks.ClassLoad = &OnClassLoad;
    callbacks.ClassPrepare = &OnClassPrepare;
    jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks));

    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_LOAD, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_PREPARE, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL);

    return JNI_OK;
}

JNIEXPORT
jint JNICALL JNI_OnLoad(JavaVM* jvm, void* reserved) {
    return JNI_VERSION_1_8;
}

// Called by the workload thread to register its JNIEnv and pthread_t.
JNIEXPORT void JNICALL
Java_TestFrameComplete_registerThread(JNIEnv* env, jclass cls) {
    target_env = env;
    target_thread = pthread_self();
}

// Called by the main thread. Sends SIGPROF to the workload thread repeatedly.
JNIEXPORT void JNICALL
Java_TestFrameComplete_runProfile(JNIEnv* env, jclass cls,
                                  jint numSamples, jint intervalUs) {
    // Install SIGPROF handler.
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigprof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, &old_sa);

    // Reset counters.
    total_samples = 0;
    success_samples = 0;
    unknown_java_samples = 0;
    other_error_samples = 0;

    // Send SIGPROF to target thread.
    for (int i = 0; i < numSamples; i++) {
        pthread_kill(target_thread, SIGPROF);
        usleep(intervalUs);
    }

    // Let the last signal be processed.
    usleep(10000);

    // Restore previous handler.
    sigaction(SIGPROF, &old_sa, NULL);
}

JNIEXPORT jint JNICALL
Java_TestFrameComplete_getTotalSamples(JNIEnv* env, jclass cls) {
    return total_samples;
}

JNIEXPORT jint JNICALL
Java_TestFrameComplete_getSuccessSamples(JNIEnv* env, jclass cls) {
    return success_samples;
}

JNIEXPORT jint JNICALL
Java_TestFrameComplete_getUnknownJavaSamples(JNIEnv* env, jclass cls) {
    return unknown_java_samples;
}

JNIEXPORT jint JNICALL
Java_TestFrameComplete_getOtherErrorSamples(JNIEnv* env, jclass cls) {
    return other_error_samples;
}

} // extern "C"
