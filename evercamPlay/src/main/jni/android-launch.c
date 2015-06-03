/* GStreamer
 *
 * Copyright (C) 2014 Sebastian Dröge <sebastian@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <pthread.h>

#include "../gst-launch-remote/gst-launch-remote.h"

GST_DEBUG_CATEGORY_STATIC (debug_category);
#define GST_CAT_DEFAULT debug_category

#define FREE_NON_NULL_STR(str) \
    if (str != NULL) {         \
        g_free(str);           \
        str = NULL;            \
    }

#define DUP_STRING(dest, src) \
    FREE_NON_NULL_STR(dest)   \
    dest = g_strdup(src)

#define PIPELINE_MAX_LEN 4096

/*
 * These macros provide a way to store the native pointer to AndroidLaunch, which might be 32 or 64 bits, into
 * a jlong, which is always 64 bits, without warnings.
 */
#if GLIB_SIZEOF_VOID_P == 8
# define GET_CUSTOM_DATA(env, thiz, fieldID) (AndroidLaunch *)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)data)
#else
# define GET_CUSTOM_DATA(env, thiz, fieldID) (AndroidLaunch *)(jint)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)(jint)data)
#endif

typedef struct _AndroidLaunch
{
  jobject app;
  GstLaunchRemote *launch;
  ANativeWindow *native_window;
} AndroidLaunch;

static pthread_t gst_app_thread;
static pthread_key_t current_jni_env;
static JavaVM *java_vm;
static jfieldID app_data_field_id;
static jmethodID set_message_method_id;
static jmethodID set_current_position_method_id;
static jmethodID on_gstreamer_initialized_method_id;
static jmethodID on_media_size_changed_method_id;
static jmethodID on_video_loaded_method_id;
static jmethodID on_error_method_id;

/* Register this thread with the VM */
static JNIEnv *
attach_current_thread (void)
{
  JNIEnv *env;
  JavaVMAttachArgs args;

  GST_DEBUG ("Attaching thread %p", g_thread_self ());
  args.version = JNI_VERSION_1_4;
  args.name = NULL;
  args.group = NULL;

  if ((*java_vm)->AttachCurrentThread (java_vm, &env, &args) < 0) {
    GST_ERROR ("Failed to attach current thread");
    return NULL;
  }

  return env;
}

/* Unregister this thread from the VM */
static void
detach_current_thread (void *env)
{
  GST_DEBUG ("Detaching thread %p", g_thread_self ());
  (*java_vm)->DetachCurrentThread (java_vm);
}

/* Retrieve the JNI environment for this thread */
static JNIEnv *
get_jni_env (void)
{
  JNIEnv *env;

  if ((env = pthread_getspecific (current_jni_env)) == NULL) {
    env = attach_current_thread ();
    pthread_setspecific (current_jni_env, env);
  }

  return env;
}

static void
set_message (const gchar * message, gpointer user_data)
{
  AndroidLaunch *app = user_data;
  JNIEnv *env = get_jni_env ();
  jstring jmessage = (*env)->NewStringUTF (env, message);

  GST_DEBUG ("Setting message to: %s", message);
  (*env)->CallVoidMethod (env, app->app, set_message_method_id, jmessage);
  if ((*env)->ExceptionCheck (env)) {
    GST_ERROR ("Failed to call Java method");
    (*env)->ExceptionClear (env);
  }
  (*env)->DeleteLocalRef (env, jmessage);
}

static void
set_error (const gchar * message, gint code, gpointer user_data)
{
  AndroidLaunch *app = user_data;
  JNIEnv *env = get_jni_env ();
  jstring jmessage = (*env)->NewStringUTF (env, message);

  GST_DEBUG ("Setting error with message %s and code %d", message, code);
  (*env)->CallVoidMethod (env, app->app, on_error_method_id, jmessage, code);
  if ((*env)->ExceptionCheck (env)) {
    GST_ERROR ("Failed to call Java method");
    (*env)->ExceptionClear (env);
  }
  (*env)->DeleteLocalRef (env, jmessage);
}

static void
set_current_position (gint position, gint duration, gpointer user_data)
{
  AndroidLaunch *app = user_data;
  JNIEnv *env = get_jni_env ();

  (*env)->CallVoidMethod (env, app->app, set_current_position_method_id,
      position, duration);
  if ((*env)->ExceptionCheck (env)) {
    GST_ERROR ("Failed to call Java method");
    (*env)->ExceptionClear (env);
  }
}

static void
media_size_changed (gint width, gint height, gpointer user_data)
{
  AndroidLaunch *app = user_data;
  JNIEnv *env = get_jni_env ();

  (*env)->CallVoidMethod (env, app->app, on_media_size_changed_method_id,
      (jint) width, (jint) height);
  if ((*env)->ExceptionCheck (env)) {
    GST_ERROR ("Failed to call Java method");
    (*env)->ExceptionClear (env);
  }
}

static void notify_about_video_loaded(gpointer user_data)
{
    GST_DEBUG("notify_about_video_loaded");
    AndroidLaunch *app = user_data;
    JNIEnv *env = get_jni_env ();

    (*env)->CallVoidMethod (env, app->app, on_video_loaded_method_id);
    if ((*env)->ExceptionCheck (env)) {
      GST_ERROR ("Failed to call Java method");
      (*env)->ExceptionClear (env);
    }
}

static void
initialized (gpointer user_data)
{
  AndroidLaunch *app = user_data;
  JNIEnv *env = get_jni_env ();

  (*env)->CallVoidMethod (env, app->app, on_gstreamer_initialized_method_id);
  if ((*env)->ExceptionCheck (env)) {
    GST_ERROR ("Failed to call Java method");
    (*env)->ExceptionClear (env);
  }
}

/*
 * Java Bindings
 */
static void
android_launch_init (JNIEnv * env, jobject thiz)
{
  GstLaunchRemoteAppContext app_context;
  AndroidLaunch *app = g_slice_new0 (AndroidLaunch);

  GST_DEBUG_CATEGORY_INIT (debug_category, "android-launch", 0,
      "Android Launch");

  app_context.app = app;
  app_context.set_message = set_message;
  app_context.set_error = set_error;
  app_context.set_current_position = set_current_position;
  app_context.initialized = initialized;
  app_context.media_size_changed = media_size_changed;
  app_context.on_video_loaded = notify_about_video_loaded;
  app->launch = gst_launch_remote_new (&app_context);

  SET_CUSTOM_DATA (env, thiz, app_data_field_id, app);
  gst_debug_set_threshold_for_name ("android-launch", GST_LEVEL_DEBUG);
  GST_DEBUG ("Created AndroidLaunch at %p", app);
  app->app = (*env)->NewGlobalRef (env, thiz);
  GST_DEBUG ("Created GlobalRef for app object at %p", app->app);
}

static void
android_launch_finalize (JNIEnv * env, jobject thiz)
{
  AndroidLaunch *app = GET_CUSTOM_DATA (env, thiz, app_data_field_id);

  if (!app)
    return;

  GST_DEBUG ("Quitting main loop...");
  FREE_NON_NULL_STR(app->launch->username);
  FREE_NON_NULL_STR(app->launch->password);
  gst_launch_remote_free (app->launch);
  GST_DEBUG ("Deleting GlobalRef for app object at %p", app->app);
  (*env)->DeleteGlobalRef (env, app->app);
  GST_DEBUG ("Freeing AndroidLaunch at %p", app);
  g_slice_free (AndroidLaunch, app);
  SET_CUSTOM_DATA (env, thiz, app_data_field_id, NULL);
  GST_DEBUG ("Done finalizing");
}

void
Java_io_evercam_androidapp_video_VideoActivity_nativeSetUsername(JNIEnv * env, jobject thiz, jstring username)
{
    AndroidLaunch *app = GET_CUSTOM_DATA (env, thiz, app_data_field_id);

    if (!app)
        return;

    const char* uname = (*env)->GetStringUTFChars(env, username, 0);
    DUP_STRING(app->launch->username, uname);
    (*env)->ReleaseStringUTFChars (env, username, uname);
}

void
Java_io_evercam_androidapp_video_VideoActivity_nativeSetPassword(JNIEnv * env, jobject thiz, jstring password)
{
    AndroidLaunch *app = GET_CUSTOM_DATA (env, thiz, app_data_field_id);

    if (!app)
      return;

    const char* pw = (*env)->GetStringUTFChars(env, password, 0);
    DUP_STRING(app->launch->password, pw);
    (*env)->ReleaseStringUTFChars (env, password, pw);
}

void
Java_io_evercam_androidapp_video_VideoActivity_nativeSetUri(JNIEnv * env, jobject thiz, jstring arg_uri)
{
    AndroidLaunch *app = GET_CUSTOM_DATA (env, thiz, app_data_field_id);

    if (!app)
      return;

    const char* uri = (*env)->GetStringUTFChars(env, arg_uri, 0);
    GST_DEBUG("uri == %s", uri);
    DUP_STRING(app->launch->uri, uri);
    (*env)->ReleaseStringUTFChars (env, arg_uri, uri);
}

void
Java_io_evercam_androidapp_video_VideoActivity_nativeSetTcpTimeout(JNIEnv * env, jobject thiz, jint value)
{
    AndroidLaunch *app = GET_CUSTOM_DATA (env, thiz, app_data_field_id);

    if (!app)
      return;

    app->launch->tcp_timeout = value;
}

void
Java_io_evercam_androidapp_video_VideoActivity_nativeRequestSample(JNIEnv * env, jobject thiz)
{
    AndroidLaunch *app = GET_CUSTOM_DATA (env, thiz, app_data_field_id);

    if (!app)
      return;

    GstSample *sample = NULL;
    g_object_get (G_OBJECT (app->launch->pipeline), "sample", &sample, NULL);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer (sample);
    GstMapInfo map;
    gst_buffer_map (buffer, &map, GST_MAP_READ);
    FILE *f = fopen("/sdcard/sample.dat", "wb");
    fwrite(map.data, map.size, 1, f);
    fclose(f);
    gst_buffer_unmap (buffer, &map);
    GST_DEBUG("sample caps %s", gst_caps_to_string(caps));
    gst_caps_unref(caps);
    gst_sample_unref(sample);

    /*GstCaps *caps = gst_caps_new_simple ("image/jpeg", NULL);
    g_signal_emit_by_name(app->launch->pipeline, "convert-sample", caps, &sample);
    gst_caps_unref(caps);

    if (sample) {
        caps = gst_sample_get_caps(sample);
        GstStructure *s = gst_caps_get_structure (caps, 0);
        int width, height;
        // we need to get the final caps on the buffer to get the size
        gboolean res = gst_structure_get_int (s, "width", &width);
        res = gst_structure_get_int (s, "height", &height);
        GST_DEBUG("Java_io_evercam_androidapp_video_VideoActivity_nativeRequestSample: width %d height %d", width, height);
        GstBuffer *buffer = gst_sample_get_buffer (sample);
        //GstMemory  *memory = gst_buffer_get_memory (buffer, 0);
        GstMapInfo map;
        gst_buffer_map (buffer, &map, GST_MAP_READ);
        //gst_memory_map (memory, &map, GST_MAP_READ);
        FILE *f = fopen("/sdcard/sample.jpeg", "wb");
        //fwrite(map.data, sizeof(map.data[0]), map.size, f);
        fwrite(map.data, map.size, 1, f);
        fclose(f);
        gst_buffer_unmap (buffer, &map);
        //gst_memory_unmap (memory, &map);
        gst_caps_unref(caps);
        gst_sample_unref(sample);
    }*/
}

static void
android_launch_play (JNIEnv * env, jobject thiz)
{
  AndroidLaunch *app = GET_CUSTOM_DATA (env, thiz, app_data_field_id);

  if (!app)
    return;

  gst_launch_remote_play (app->launch);
}

static void
android_launch_pause (JNIEnv * env, jobject thiz)
{
  AndroidLaunch *app = GET_CUSTOM_DATA (env, thiz, app_data_field_id);

  if (!app)
    return;

  gst_launch_remote_pause (app->launch);
}

static void
android_launch_stop (JNIEnv * env, jobject thiz)
{
  AndroidLaunch *app = GET_CUSTOM_DATA (env, thiz, app_data_field_id);

  if (!app)
    return;

  gst_launch_remote_stop (app->launch);
}

static jboolean
android_launch_class_init (JNIEnv * env, jclass klass)
{
  app_data_field_id = (*env)->GetFieldID (env, klass, "native_app_data", "J");
  set_message_method_id =
      (*env)->GetMethodID (env, klass, "setMessage", "(Ljava/lang/String;)V");
  set_current_position_method_id =
      (*env)->GetMethodID (env, klass, "setCurrentPosition", "(II)V");
  on_gstreamer_initialized_method_id =
      (*env)->GetMethodID (env, klass, "onGStreamerInitialized", "()V");
  on_media_size_changed_method_id =
      (*env)->GetMethodID (env, klass, "onMediaSizeChanged", "(II)V");
  on_video_loaded_method_id =
          (*env)->GetMethodID (env, klass, "onVideoLoaded", "()V");
  on_error_method_id =
          (*env)->GetMethodID (env, klass, "onError", "(Ljava/lang/String;I)V");

  if (!app_data_field_id || !set_message_method_id
      || !on_gstreamer_initialized_method_id || !on_media_size_changed_method_id
      || !set_current_position_method_id) {
    /* We emit this message through the Android log instead of the GStreamer log because the later
     * has not been initialized yet.
     */
    __android_log_print (ANDROID_LOG_ERROR, "android-launch",
        "The calling class does not implement all necessary interface methods");
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

static void
android_launch_surface_init (JNIEnv * env, jobject thiz, jobject surface)
{
  AndroidLaunch *app = GET_CUSTOM_DATA (env, thiz, app_data_field_id);
  ANativeWindow *new_native_window;

  if (!app)
    return;

  new_native_window = ANativeWindow_fromSurface (env, surface);
  GST_DEBUG ("Received surface %p (native window %p)", surface,
      new_native_window);

  if (app->native_window) {
    ANativeWindow_release (app->native_window);
  }

  app->native_window = new_native_window;
  gst_launch_remote_set_window_handle (app->launch, (guintptr) new_native_window);
}

static void
android_launch_surface_finalize (JNIEnv * env, jobject thiz)
{
  AndroidLaunch *app = GET_CUSTOM_DATA (env, thiz, app_data_field_id);

  if (!app)
    return;

  GST_DEBUG ("Releasing Native Window %p", app->native_window);

  gst_launch_remote_set_window_handle (app->launch, (guintptr) NULL);

  ANativeWindow_release (app->native_window);
  app->native_window = NULL;
}

/* List of implemented native methods */
static JNINativeMethod native_methods[] = {
  {"nativeInit", "()V", (void *) android_launch_init},
  {"nativeFinalize", "()V", (void *) android_launch_finalize},
  {"nativePlay", "()V", (void *) android_launch_play},
  {"nativePause", "()V", (void *) android_launch_pause},
  {"nativeStop", "()V", (void *) android_launch_stop},
  {"nativeSurfaceInit", "(Ljava/lang/Object;)V",
      (void *) android_launch_surface_init},
  {"nativeSurfaceFinalize", "()V", (void *) android_launch_surface_finalize},
  {"nativeClassInit", "()Z", (void *) android_launch_class_init}
};

/* Library initializer */
jint
JNI_OnLoad (JavaVM * vm, void *reserved)
{
  JNIEnv *env = NULL;

  java_vm = vm;

  if ((*vm)->GetEnv (vm, (void **) &env, JNI_VERSION_1_4) != JNI_OK) {
    __android_log_print (ANDROID_LOG_ERROR, "android-launch",
        "Could not retrieve JNIEnv");
    return 0;
  }
  jclass klass =
      (*env)->FindClass (env, "io/evercam/androidapp/video/VideoActivity");
  (*env)->RegisterNatives (env, klass, native_methods,
      G_N_ELEMENTS (native_methods));

  pthread_key_create (&current_jni_env, detach_current_thread);

  return JNI_VERSION_1_4;
}
