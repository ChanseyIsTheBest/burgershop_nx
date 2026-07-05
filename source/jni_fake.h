/* jni_fake.h -- fake JNI environment for the SexyAppFramework engine
 *               (libSexyAndroid.so, Burger Shop)
 *
 * The PopCap/SexyAppFramework engine does the heavy lifting natively: it
 * renders through GLES1, decodes PNG/JPEG with its statically-linked
 * libpng/libjpeg/zlib, reads its data straight out of the GoBit pak via fopen,
 * and plays audio through BASS. The Java side it calls back into is therefore
 * thin -- platform "manager" services (ads, cloud, analytics, social, the
 * Java SoundPool/MediaPlayer helpers). Those manager methods are obfuscated in
 * this build, so the dispatcher returns signature-appropriate safe defaults
 * and logs every unhandled call (name + signature) to make on-device mapping
 * straightforward.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *
// Set when the engine asks Android to show the soft keyboard (JNI "ShowKeyboard"
// with show=true); main.c services it with the Switch software keyboard.
extern volatile int g_kbd_requested;

// set if the engine ever asks the activity to finish
extern volatile int jni_quit_requested;

void jni_init(void);

// the fake activity instance / class handed to the JNI entry points
void *jni_make_thiz(void);

// constructors for fake Java objects
void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);

// Force a specific return value for an (obfuscated) boolean/int manager method
// by name -- handy during bring-up when a particular query must answer "true"
// for the engine to proceed. Returns 0 on success, -1 if the table is full.
// (The fake-JNI logs every unhandled call so you can discover the names.)
int jni_force_int_return(const char *method_name, long value);

#endif
