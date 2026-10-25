/* MHFU-Android -- what the Android app's dialogs (MhfuActivity: Multiplayer,
 * RetroAchievements) call in the game: the lobby (lobby.h) and the sign-in
 * (achievements.h). Static natives of com.pateeerq.mhfu.MhfuActivity; the
 * lobby ones block for a few seconds, so Java calls them off its UI thread. */
#ifdef __ANDROID__

#include <jni.h>
#include <stdio.h>
#include <string.h>

#include "achievements.h"
#include "lobby.h"

#define FN(name) JNICALL Java_com_pateeerq_mhfu_MhfuActivity_##name

static void str(JNIEnv *env, jstring s, char *out, size_t cap) {
    out[0] = 0;
    if (!s) return;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    if (c) { snprintf(out, cap, "%s", c); (*env)->ReleaseStringUTFChars(env, s, c); }
}

/* null when it worked, else the message for the player. */
JNIEXPORT jstring FN(lobbyCreate)(JNIEnv *env, jclass cls, jstring pass, jboolean internet) {
    (void)cls;
    char p[16], err[256];
    str(env, pass, p, sizeof p);
    return mhfu_lobby_create(p, internet, err, sizeof err) == 0 ? NULL : (*env)->NewStringUTF(env, err);
}

JNIEXPORT jboolean FN(lobbyHasRelay)(JNIEnv *env, jclass cls) { (void)env; (void)cls; return (jboolean)mhfu_lobby_has_relay(); }

JNIEXPORT jstring FN(lobbyJoin)(JNIEnv *env, jclass cls, jstring code, jstring pass) {
    (void)cls;
    char c[32], p[16], err[256];
    str(env, code, c, sizeof c);
    str(env, pass, p, sizeof p);
    return mhfu_lobby_join(c, p, err, sizeof err) == 0 ? NULL : (*env)->NewStringUTF(env, err);
}

JNIEXPORT void FN(lobbyLeave)(JNIEnv *env, jclass cls) { (void)env; (void)cls; mhfu_lobby_leave(); }

/* One "code\tname\tplayers\tlocked\tinternet" per open room. */
JNIEXPORT jobjectArray FN(lobbyList)(JNIEnv *env, jclass cls) {
    (void)cls;
    static mhfu_room rooms[32];
    const int n = mhfu_lobby_list(rooms, 32);
    jobjectArray out = (*env)->NewObjectArray(env, n > 0 ? n : 0, (*env)->FindClass(env, "java/lang/String"), NULL);
    for (int i = 0; i < n; i++) {
        char line[96];
        snprintf(line, sizeof line, "%s\t%s\t%d\t%d\t%d", rooms[i].code, rooms[i].name, rooms[i].players,
                 rooms[i].locked, rooms[i].internet);
        jstring s = (*env)->NewStringUTF(env, line);
        (*env)->SetObjectArrayElement(env, out, i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    return out;
}

JNIEXPORT jstring FN(lobbyStatus)(JNIEnv *env, jclass cls) {
    (void)cls;
    char s[128];
    mhfu_lobby_status(s, sizeof s);
    return (*env)->NewStringUTF(env, s);
}

JNIEXPORT jstring FN(lobbyCode)(JNIEnv *env, jclass cls) {
    (void)cls;
    char c[8];
    mhfu_lobby_code(c);
    return (*env)->NewStringUTF(env, c);
}

JNIEXPORT jstring FN(lobbyNick)(JNIEnv *env, jclass cls) { (void)cls; return (*env)->NewStringUTF(env, mhfu_lobby_nick()); }

JNIEXPORT void FN(lobbySetNick)(JNIEnv *env, jclass cls, jstring nick) {
    (void)cls;
    char n[64];
    str(env, nick, n, sizeof n);
    if (n[0]) mhfu_lobby_set_nick(n);
}

JNIEXPORT void FN(achLogin)(JNIEnv *env, jclass cls, jstring user, jstring token) {
    (void)cls;
    char u[64], t[128];
    str(env, user, u, sizeof u);
    str(env, token, t, sizeof t);
    if (u[0] && t[0]) mhfu_ach_login(u, t);
}

JNIEXPORT void FN(achLogout)(JNIEnv *env, jclass cls) { (void)env; (void)cls; mhfu_ach_logout(); }

JNIEXPORT jstring FN(achUser)(JNIEnv *env, jclass cls) { (void)cls; return (*env)->NewStringUTF(env, mhfu_ach_user()); }

#endif
