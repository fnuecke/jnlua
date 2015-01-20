/*
 * $Id$
 * See LICENSE.txt for license terms.
 */

#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jni.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/* Include uintptr_t */
#ifdef LUA_WIN
#include <stddef.h>
#endif
#ifdef LUA_USE_POSIX
#include <stdint.h>
#endif

/* ---- Definitions ---- */
#define JNLUA_APIVERSION 3
#define JNLUA_JNIVERSION JNI_VERSION_1_6
#define JNLUA_JAVASTATE "jnlua.JavaState"
#define JNLUA_OBJECT "jnlua.Object"
#define JNLUA_MINSTACK LUA_MINSTACK
#define JNLUA_PCALL(L, nargs, nresults) {\
	int status = lua_pcall(L, (nargs), (nresults), 0);\
	if (status != LUA_OK) {\
		throw(L, status);\
	}\
}

/* ---- Types ---- */
/* Structure for reading and writing Java streams. */
typedef struct StreamStruct  {
	jobject stream;
	jbyteArray byte_array;
	jbyte* bytes;
	jboolean is_copy;
} Stream;

/* ---- JNI helpers ---- */
static jclass referenceclass(JNIEnv *env, const char *className);
static jbyteArray newbytearray(JNIEnv *env, jsize length);
static const char *getstringchars(JNIEnv *env, jstring string);
static void releasestringchars(JNIEnv *env, jstring string, const char *chars);

/* ---- Java state operations ---- */
static lua_State *getluastate(JNIEnv *env, jobject javastate);
static void setluastate(JNIEnv *env, jobject javastate, lua_State *L);
static lua_State *getluathread(JNIEnv *env, jobject javastate);
static void setluathread(JNIEnv *env, jobject javastate, lua_State *L);
static int getyield(JNIEnv *env, jobject javastate);
static void setyield(JNIEnv *env, jobject javastate, int yield);
static lua_Debug *getluadebug(JNIEnv *env, jobject javadebug);
static void setluadebug(JNIEnv *env, jobject javadebug, lua_Debug *ar);
static JNIEnv *getthreadenv();

/* ---- Memory use control ---- */
static void getluamemory(JNIEnv *env, jobject obj, jint *total, jint *used);
static void setluamemory(JNIEnv *env, jobject obj, jint used);

/* ---- Checks ---- */
static int validindex(lua_State *L, int index);
static int checkstack(lua_State *L, int space);
static int checkindex(lua_State *L, int index);
static int checkrealindex(lua_State *L, int index);
static int checktype(lua_State *L, int index, int type);
static int checknelems(lua_State *L, int n);
static int checknotnull (void *object);
static int checkarg(int cond, const char *msg);
static int checkstate(int cond, const char *msg);
static int check(int cond, jthrowable throwable_class, const char *msg);

/* ---- Java objects and functions ---- */
static void pushjavaobject(lua_State *L, jobject object);
static jobject tojavaobject(lua_State *L, int index, jclass class);
static jstring tostring(lua_State *L, int index);
static int gcjavaobject(lua_State *L);
static int calljavafunction(lua_State *L);

/* ---- Error handling ---- */
static int messagehandler(lua_State *L);
static int isrelevant(lua_Debug *ar);
static void throw(lua_State *L, int status);

/* ---- Stream adapters ---- */
static const char *readhandler(lua_State *L, void *ud, size_t *size);
static int writehandler(lua_State *L, const void *data, size_t size, void *ud);

/* ---- Variables ---- */
static jclass luastate_class = NULL;
static jfieldID luastate_id = 0;
static jfieldID luathread_id = 0;
static jfieldID luamemorytotal_id = 0;
static jfieldID luamemoryused_id = 0;
static jfieldID yield_id = 0;
static jclass luadebug_class = NULL;
static jmethodID luadebug_init_id = 0;
static jfieldID luadebug_field_id = 0;
static jclass javafunction_interface = NULL;
static jmethodID invoke_id = 0;
static jclass luaruntimeexception_class = NULL;
static jmethodID luaruntimeexception_id = 0;
static jmethodID setluaerror_id = 0;
static jclass luasyntaxexception_class = NULL;
static jmethodID luasyntaxexception_id = 0;
static jclass luamemoryallocationexception_class = NULL;
static jmethodID luamemoryallocationexception_id = 0;
static jclass luagcmetamethodexception_class = NULL;
static jmethodID luagcmetamethodexception_id = 0;
static jclass luamessagehandlerexception_class = NULL;
static jmethodID luamessagehandlerexception_id = 0;
static jclass luastacktraceelement_class = NULL;
static jmethodID luastacktraceelement_id = 0;
static jclass luaerror_class = NULL;
static jmethodID luaerror_id = 0;
static jmethodID setluastacktrace_id = 0;
static jclass nullpointerexception_class = NULL;
static jclass illegalargumentexception_class = NULL;
static jclass illegalstateexception_class = NULL;
static jclass error_class = NULL;
static jclass integer_class = NULL;
static jmethodID valueof_integer_id = 0;
static jclass double_class = NULL;
static jmethodID valueof_double_id = 0;
static jclass inputstream_class = NULL;
static jmethodID read_id = 0;
static jclass outputstream_class = NULL;
static jmethodID write_id = 0;
static jclass ioexception_class = NULL;
static int initialized = 0;
static JavaVM *java_vm = NULL;

/* ---- Fields ---- */
/* lua_registryindex() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1registryindex(JNIEnv *env, jobject obj) {
	return (jint) LUA_REGISTRYINDEX;
}

/* lua_version() */
JNIEXPORT jstring JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1version(JNIEnv *env, jobject obj) {
	const char *luaVersion;
	
	luaVersion = LUA_VERSION;
	if (strncmp(luaVersion, "Lua+Eris ", 9) == 0) {
		luaVersion += 9;
	}
	return (*env)->NewStringUTF(env, luaVersion); 
}

/* ---- Life cycle ---- */
/*
 * lua_newstate()
 */
static int newstate_protected (lua_State *L) {
	JNIEnv *thread_env = getthreadenv();
	jobject *ref;
	jobject newstate_obj = (jobject)lua_touserdata(L, 1);
	lua_pop(L, 1);
	
	/* Set the Java state in the Lua state. */
	ref = lua_newuserdata(L, sizeof(jobject));
	lua_createtable(L, 0, 1);
	lua_pushboolean(L, 1); /* weak global reference */
	lua_pushcclosure(L, gcjavaobject, 1);
	lua_setfield(L, -2, "__gc");
	*ref = (*thread_env)->NewWeakGlobalRef(thread_env, newstate_obj);
	if (!*ref) {
		lua_pushliteral(L, "JNI error: NewWeakGlobalRef() failed setting up Lua state");
		return lua_error(L);
	}
	lua_setmetatable(L, -2);
	lua_setfield(L, LUA_REGISTRYINDEX, JNLUA_JAVASTATE);
	
	/*
	 * Create the meta table for Java objects and return it. Population will
	 * be finished on the Java side.
	 */
	luaL_newmetatable(L, JNLUA_OBJECT);
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");
	lua_pushboolean(L, 0); /* non-weak global reference */
	lua_pushcclosure(L, gcjavaobject, 1);
	lua_setfield(L, -2, "__gc");
	return 1;
}
/* Get Java state from Lua state. */
static jobject getjavastate(lua_State* L) {
	jobject obj = NULL;
	lua_getfield(L, LUA_REGISTRYINDEX, JNLUA_JAVASTATE);
	if (lua_isuserdata(L, -1)) {
		obj = *(jobject *) lua_touserdata(L, -1);
	} /* else: Java state has been cleared as the Java VM was destroyed. */
	lua_pop(L, 1);
	return obj;
}
/* This default allocator is set while inside the controlled allocator to
   avoid recursion when looking up the Java state from the Lua state. */
static void* l_alloc_unchecked (void *ud, void *ptr, size_t osize, size_t nsize) {
	if (nsize == 0) {
		free(ptr);
		return NULL;
	} else {
		return realloc(ptr, nsize);
	}
}
/* This custom allocator ensures a VM won't exceed its allowed memory use. */
static void* l_alloc_checked (void *ud, void *ptr, size_t osize, size_t nsize) {
	JNIEnv *thread_env = getthreadenv();
	lua_State *L = (lua_State*)ud;
	jobject obj;
	lua_setallocf(L, l_alloc_unchecked, NULL);
	obj = getjavastate(L);
	lua_setallocf(L, l_alloc_checked, L);
	if (obj) {
		/* We have a Java state, enforce memory control. */
		jint total = 0, used = 0;
		getluamemory(thread_env, obj, &total, &used);
		if (nsize == 0) {
			/* Free a block of memory. */
			free(ptr);
			setluamemory(thread_env, obj, used - osize);
			return NULL;
		} else {
			int delta;
			if (ptr == NULL) {
				delta = nsize;
			} else {
				delta = nsize - osize;
			}
			/* Lua expects reduction to not fail, so we must allow that even if it
			   exceeds our current memory cap. */
			if (total <= 0 || delta <= 0 || total - used >= delta) {
				setluamemory(thread_env, obj, used + delta);
				return realloc(ptr, nsize);
			} else {
				return NULL;
			}
		}
	}
	else {
		/* State was cleaned up on the Java side, fall back to default. */
		return l_alloc_unchecked(ud, ptr, osize, nsize);
	}
}
static lua_State *controlled_newstate (JNIEnv *env, jobject obj) {
	lua_State *L = luaL_newstate();
	if (L) {
		jint total = 0, used = 0;
		getluamemory(env, obj, &total, &used);
		if (total > 0) {
			lua_setallocf(L, l_alloc_checked, L);
		}
	}
	return L;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1newstate (JNIEnv *env, jobject obj, int apiversion, jlong existing) {
	lua_State *L;
	
	/* Initialized? */
	if (!initialized) {
		return;
	}
	
	/* API version? */
	if (apiversion != JNLUA_APIVERSION) {
		return;
	}

	/* Create or attach to Lua state. */
	L = !existing ? controlled_newstate(env, obj) : (lua_State *) (uintptr_t) existing;
	if (!L) {
		return;
	}
	
	/* Setup Lua state. */
	if (checkstack(L, JNLUA_MINSTACK)) {
		lua_pushcfunction(L, newstate_protected);
		lua_pushlightuserdata(L, (void*)obj);
		JNLUA_PCALL(L, 1, 1);
	}
	if ((*env)->ExceptionCheck(env)) {
		if (!existing) {
			lua_setallocf(L, l_alloc_unchecked, NULL);
			setluamemory(env, obj, 0);
			lua_close(L);
		}
		return;
	}
	
	/* Set the Lua state in the Java state. */
	setluathread(env, obj, L);
	setluastate(env, obj, L);
}

/* lua_close() */
static int close_protected (lua_State *L) {
	/* Unset the Java state in the Lua state. */
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, JNLUA_JAVASTATE);
	
	return 0;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1close (JNIEnv *env, jobject obj, jboolean ownstate) {
	lua_State *L = getluastate(env, obj), *T;
	lua_Debug ar;
	if (ownstate) {
		/* Can close? */
		T = getluathread(env, obj);
		if (L != T || lua_getstack(L, 0, &ar)) {
			return;
		}
		
		/* Unset the Lua state in the Java state. */
		setluastate(env, obj, NULL);
		setluathread(env, obj, NULL);
		
		/* Close Lua state. */
		lua_setallocf(L, l_alloc_unchecked, NULL);
		setluamemory(env, obj, 0);
		lua_close(L);
	} else {
		/* Can close? */
		if (!lua_checkstack(L, JNLUA_MINSTACK)) {
			return;
		}
		
		/* Cleanup Lua state. */
		lua_pushcfunction(L, close_protected);
		JNLUA_PCALL(L, 0, 0);
		if ((*env)->ExceptionCheck(env)) {
			return;
		}
		
		/* Unset the Lua state in the Java state. */
		setluastate(env, obj, NULL);
		setluathread(env, obj, NULL);
	}
}

/* lua_gc() */
static int gc_protected (lua_State *L) {
	lua_pushinteger(L, lua_gc(L, lua_tointeger(L, 1), lua_tointeger(L, 2)));
	return 1;
}
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1gc (JNIEnv *env, jobject obj, jint what, jint data) {
	lua_State *L = getluathread(env, obj);
	jint result = 0;
	if(checkstack(L, JNLUA_MINSTACK)) {
		lua_pushcfunction(L, gc_protected);
		lua_pushinteger(L, what);
		lua_pushinteger(L, data);
		JNLUA_PCALL(L, 2, 1);
		result = (jint)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	return result;
}

/* ---- Registration ---- */
/* lua_openlib() */
static int openlib_protected (lua_State *L) {
	const char *libname;
	lua_CFunction openfunc;

	switch (lua_tointeger(L, 1)) {
	case 0:
		libname = "_G";
		openfunc = luaopen_base;
		break;
	case 1:
		libname = LUA_LOADLIBNAME;
		openfunc = luaopen_package;
		break;
	case 2:
		libname = LUA_COLIBNAME;
		openfunc = luaopen_coroutine;
		break;
	case 3:
		libname = LUA_TABLIBNAME;
		openfunc = luaopen_table;
		break;
	case 4:
		libname = LUA_IOLIBNAME;
		openfunc = luaopen_io;
		break;
	case 5:
		libname = LUA_OSLIBNAME;
		openfunc = luaopen_os;
		break;
	case 6:
		libname = LUA_STRLIBNAME;
		openfunc = luaopen_string;
		break;
	case 7:
		libname = LUA_BITLIBNAME;
		openfunc = luaopen_bit32;
		break;
	case 8:
		libname = LUA_MATHLIBNAME;
		openfunc = luaopen_math;
		break;
	case 9:
		libname = LUA_DBLIBNAME;
		openfunc = luaopen_debug;
		break;
	case 10:
		libname = LUA_ERISLIBNAME;
		openfunc = luaopen_eris;
		break;
	default:
		return 0;
	}
	luaL_requiref(L, libname, openfunc, 1);
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1openlib (JNIEnv *env, jobject obj, jint lib) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checkarg(lib >= 0 && lib <= 10, "illegal library")) {
		lua_pushcfunction(L, openlib_protected);
		lua_pushinteger(L, lib);
		JNLUA_PCALL(L, 1, 1);
	}
}

/* ---- Load and dump ---- */
/* lua_load() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1load (JNIEnv *env, jobject obj, jobject inputStream, jstring chunkname, jstring mode) {
	lua_State *L = getluathread(env, obj);
	const char *chunkname_utf = NULL, *mode_utf = NULL;
	Stream stream = { inputStream, NULL, NULL, 0 };
	int status;
	if (checkstack(L, JNLUA_MINSTACK)
			&& (chunkname_utf = getstringchars(env, chunkname))
			&& (mode_utf = getstringchars(env, mode)) 
			&& (stream.byte_array = newbytearray(env, 1024))) {
		status = lua_load(L, readhandler, &stream, chunkname_utf, mode_utf);
		if (status != LUA_OK) {
			throw(L, status);
		}
	}
	if (stream.bytes) {
		(*env)->ReleaseByteArrayElements(env, stream.byte_array, stream.bytes, JNI_ABORT);
	}
	if (stream.byte_array) {
		(*env)->DeleteLocalRef(env, stream.byte_array);
	}
	if (chunkname_utf) {
		releasestringchars(env, chunkname, chunkname_utf);
	}
	if (mode_utf) {
		releasestringchars(env, mode, mode_utf);
	}
}

/* lua_dump() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1dump (JNIEnv *env, jobject obj, jobject outputStream) {
	lua_State *L = getluathread(env, obj);
	Stream stream = { outputStream, NULL, NULL, 0 };
	if (checkstack(L, JNLUA_MINSTACK)
			&& checknelems(L, 1)
			&& (stream.byte_array = newbytearray(env, 1024))) {
		lua_dump(L, writehandler, &stream, 0);
	}
	if (stream.bytes) {
		(*env)->ReleaseByteArrayElements(env, stream.byte_array, stream.bytes, JNI_ABORT);
	}
	if (stream.byte_array) {
		(*env)->DeleteLocalRef(env, stream.byte_array);
	}
}

/* ---- Call ---- */
/* lua_pcall() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1pcall (JNIEnv *env, jobject obj, jint nargs, jint nresults) {
	lua_State *L = getluathread(env, obj);
	int index, status;
	if (checkarg(nargs >= 0, "illegal argument count")
			&& checknelems(L, nargs + 1)
			&& checkarg(nresults >= 0 || nresults == LUA_MULTRET, "illegal return count")
			&& (nresults == LUA_MULTRET || checkstack(L, nresults - (nargs + 1)))) {
		index = lua_absindex(L, -nargs - 1);
		lua_pushcfunction(L, messagehandler);
		lua_insert(L, index);
		status = lua_pcall(L, nargs, nresults, index);
		lua_remove(L, index);
		if (status != LUA_OK) {
			throw(L, status);
		}
	}
}

/* ---- Global ---- */
/* lua_getglobal() */
static int getglobal_protected (lua_State *L) {
	lua_getglobal(L, (const char*)lua_touserdata(L, 1));
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1getglobal (JNIEnv *env, jobject obj, jstring name) {
	lua_State *L = getluathread(env, obj);
	const char *getglobal_name = NULL;
	if (checkstack(L, JNLUA_MINSTACK)
			&& (getglobal_name = getstringchars(env, name))) {
		lua_pushcfunction(L, getglobal_protected);
		lua_pushlightuserdata(L, (void*)getglobal_name);
		JNLUA_PCALL(L, 1, 1);
	}
	if (getglobal_name) {
		releasestringchars(env, name, getglobal_name);
	}
}

/* lua_setglobal() */
static int setglobal_protected (lua_State *L) {
	lua_setglobal(L, (const char*)lua_touserdata(L, 1));
	return 0;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1setglobal (JNIEnv *env, jobject obj, jstring name) {
	lua_State *L = getluathread(env, obj);
	const char *setglobal_name = NULL;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checknelems(L, 1)
			&& (setglobal_name = getstringchars(env, name))) {
		lua_pushcfunction(L, setglobal_protected);
		lua_insert(L, -2);
		lua_pushlightuserdata(L, (void*)setglobal_name);
		lua_insert(L, -2);
		JNLUA_PCALL(L, 2, 0);
	}
	if (setglobal_name) {
		releasestringchars(env, name, setglobal_name);
	}
}

/* ---- Stack push ---- */
/* lua_pushboolean() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1pushboolean (JNIEnv *env, jobject obj, jint b) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)) {
		lua_pushboolean(L, b);
	}
}

/* lua_pushbytearray() */
static int pushbytearray_protected (lua_State *L) {
	lua_pushlstring(L, (jbyte*)lua_touserdata(L, 1), (jsize)lua_tointeger(L, 2));
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1pushbytearray (JNIEnv *env, jobject obj, jbyteArray ba) {
	lua_State *L = getluathread(env, obj);
	jbyte *pushbytearray_b = NULL;
	jsize pushbytearray_length;
	if (checkstack(L, JNLUA_MINSTACK)
			&& (pushbytearray_b = (*env)->GetByteArrayElements(env, ba, NULL))) {
		pushbytearray_length = (*env)->GetArrayLength(env, ba);
		lua_pushcfunction(L, pushbytearray_protected);
		lua_pushlightuserdata(L, (void*)pushbytearray_b);
		lua_pushinteger(L, pushbytearray_length);
		JNLUA_PCALL(L, 2, 1);
	}
	if (pushbytearray_b) {
		(*env)->ReleaseByteArrayElements(env, ba, pushbytearray_b, JNI_ABORT);
	}
}

/* lua_pushinteger() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1pushinteger (JNIEnv *env, jobject obj, jint n) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)) {
		lua_pushinteger(L, n);
	}
}

/* lua_pushjavafunction() */
static int pushjavafunction_protected (lua_State *L) {
	pushjavaobject(L, (jobject)lua_touserdata(L, 1));
	lua_pushcclosure(L, calljavafunction, 1);
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1pushjavafunction (JNIEnv *env, jobject obj, jobject f) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checknotnull(f)) {
		lua_pushcfunction(L, pushjavafunction_protected);
		lua_pushlightuserdata(L, (void*)f);
		JNLUA_PCALL(L, 1, 1);
	}
}

/* lua_pushjavaobject() */
static int pushjavaobject_protected (lua_State *L) {
	pushjavaobject(L, (jobject)lua_touserdata(L, 1));
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1pushjavaobject (JNIEnv *env, jobject obj, jobject object) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checknotnull(object)) {
		lua_pushcfunction(L, pushjavaobject_protected);
		lua_pushlightuserdata(L, (void*)object);
		JNLUA_PCALL(L, 1, 1);
	}
}

/* lua_pushnil() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1pushnil (JNIEnv *env, jobject obj) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)) {
		lua_pushnil(L);
	}
}

/* lua_pushnumber() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1pushnumber (JNIEnv *env, jobject obj, jdouble n) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)) {
		lua_pushnumber(L, n);
	}
}

/* lua_pushstring() */
static int pushstring_protected (lua_State *L) {
	lua_pushlstring(L, (const char*)lua_touserdata(L, 1), (jsize)lua_tointeger(L, 2));
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1pushstring (JNIEnv *env, jobject obj, jstring s) {
	lua_State *L = getluathread(env, obj);
	const char *pushstring_s = NULL;
	if (checkstack(L, JNLUA_MINSTACK)
			&& (pushstring_s = getstringchars(env, s))) {
		jsize pushstring_length = (*env)->GetStringUTFLength(env, s);
		lua_pushcfunction(L, pushstring_protected);
		lua_pushlightuserdata(L, (void*)pushstring_s);
		lua_pushinteger(L, pushstring_length);
		JNLUA_PCALL(L, 2, 1);
	}
	if (pushstring_s) {
		releasestringchars(env, s, pushstring_s);
	}
}

/* ---- Stack type test ---- */
/* lua_isboolean() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1isboolean (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index)) {
		return 0;
	}
	return (jint) lua_isboolean(L, index);
}

/* lua_iscfunction() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1iscfunction (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	lua_CFunction c_function = NULL;
	if (!validindex(L, index)) {
		return 0;
	}
	c_function = lua_tocfunction(L, index);
	return (jint) (c_function != NULL && c_function != calljavafunction);
}

/* lua_isfunction() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1isfunction (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index)) {
		return 0;
	}
	return (jint) lua_isfunction(L, index);
}

/* lua_isjavafunction() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1isjavafunction (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index)) {
		return 0;
	}
	return (jint) (lua_tocfunction(L, index) == calljavafunction);
}

/* lua_isjavaobject() */
static int isjavaobject_protected (lua_State *L) {
	lua_pushboolean(L, tojavaobject(L, 1, NULL) != NULL);
	return 1;
}
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1isjavaobject (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	jint isjavaobject_result = 0;
	if (!validindex(L, index)) {
		return 0;
	}
	if (checkstack(L, JNLUA_MINSTACK)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, isjavaobject_protected);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 1, 1);
		isjavaobject_result = (jint)lua_toboolean(L, -1);
		lua_pop(L, 1);
	}
	return isjavaobject_result;
}

/* lua_isnil() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1isnil (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index)) {
		return 0;
	}
	return (jint) lua_isnil(L, index);
}

/* lua_isnone() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1isnone (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	return (jint) !validindex(L, index);
}

/* lua_isnoneornil() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1isnoneornil (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index)) {
		return 1;
	}
	return (jint) lua_isnil(L, index);
}

/* lua_isnumber() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1isnumber (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index)) {
		return 0;
	}
	return (jint) lua_isnumber(L, index);
}

/* lua_isstring() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1isstring (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index)) {
		return 0;
	}
	return (jint) lua_isstring(L, index);
}

/* lua_istable() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1istable (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index)) {
		return 0;
	}
	return lua_istable(L, index);
}

/* lua_isthread() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1isthread (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index)) {
		return 0;
	}
	return (jint) lua_isthread(L, index);
}

/* ---- Stack query ---- */
/* lua_compare() */
static int compare_protected (lua_State *L) {
	lua_pushboolean(L, lua_compare(L, 2, 3, lua_tointeger(L, 1)));
	return 1;
}
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1compare (JNIEnv *env, jobject obj, jint index1, jint index2, jint op) {
	lua_State *L = getluathread(env, obj);
	jint compare_result = 0;
	if (!validindex(L, index1) || !validindex(L, index2)) {
		return (jint) 0;
	}
	if (checkstack(L, JNLUA_MINSTACK)) {
		index1 = lua_absindex(L, index1);
		index2 = lua_absindex(L, index2);
		lua_pushcfunction(L, compare_protected);
		lua_pushinteger(L, op);
		lua_pushvalue(L, index1);
		lua_pushvalue(L, index2);
		JNLUA_PCALL(L, 3, 1);
		compare_result = (jint)lua_toboolean(L, -1);
		lua_pop(L, 1);
	}
	return compare_result;
}

/* lua_rawequal() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1rawequal (JNIEnv *env, jobject obj, jint index1, jint index2) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index1) || !validindex(L, index2)) {
		return (jint) 0;
	}
	return (jint) lua_rawequal(L, index1, index2);
}

/* lua_rawlen() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1rawlen (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	size_t result = 0;
	if (checkindex(L, index)) {
		result = lua_rawlen(L, index);
	}
	return (jint) result;
}

/* lua_toboolean() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1toboolean (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index)) {
		return 0;
	}
	return lua_toboolean(L, index);
}

/* lua_tobytearray() */
static int tobytearray_protected (lua_State *L) {
	size_t tobytearray_length;
	const char *tobytearray_result = lua_tolstring(L, 1, &tobytearray_length);
	lua_pushlightuserdata(L, (void*)tobytearray_result);
	lua_pushinteger(L, tobytearray_length);
	return 2;
}
JNIEXPORT jbyteArray JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1tobytearray (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	jbyteArray ba;
	jbyte *b;
	size_t tobytearray_length;
	const char *tobytearray_result = NULL;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checkindex(L, index)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, tobytearray_protected);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 1, 2);
		tobytearray_result = (const char*)lua_touserdata(L, -2);
		tobytearray_length = (lua_Unsigned)lua_tointeger(L, -1);
		lua_pop(L, 2);
	}
	if (!tobytearray_result) {
		return NULL;
	}
	ba = (*env)->NewByteArray(env, (jsize) tobytearray_length);
	if (!ba) {
		return NULL;
	}
	b = (*env)->GetByteArrayElements(env, ba, NULL);
	if (!b) {
		return NULL;
	}
	memcpy(b, tobytearray_result, tobytearray_length);
	(*env)->ReleaseByteArrayElements(env, ba, b, 0);
	return ba;
}

/* lua_tointeger() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1tointeger (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	lua_Integer result = 0;
	if (checkindex(L, index)) {
		result = lua_tointeger(L, index);
	}
	return (jint) result;
}

/* lua_tointegerx() */
JNIEXPORT jobject JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1tointegerx (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	lua_Integer result = 0;
	int isnum = 0;
	if (checkindex(L, index)) {
		result = lua_tointegerx(L, index, &isnum);
	}
	return isnum ? (*env)->CallStaticObjectMethod(env, integer_class, valueof_integer_id, (jint) result) : NULL;
}

/* lua_tojavafunction() */
static int tojavafunction_protected (lua_State *L) {
	if (lua_tocfunction(L, 1) == calljavafunction) {
		if (lua_getupvalue(L, 1, 1)) {
			lua_pushlightuserdata(L, (void*)tojavaobject(L, -1, javafunction_interface));
			return 1;
		}
	}
	return 0;
}
JNIEXPORT jobject JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1tojavafunction (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	jobject tojavafunction_result = NULL;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checkindex(L, index)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, tojavafunction_protected);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 1, 1);
		tojavafunction_result = (jobject)lua_touserdata(L, -1);
		lua_pop(L, 1);
	}
	return tojavafunction_result;
}

/* lua_tojavaobject() */
static int tojavaobject_protected (lua_State *L) {
	lua_pushlightuserdata(L, (void*)tojavaobject(L, 1, NULL));
	return 1;
}
JNIEXPORT jobject JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1tojavaobject (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	jobject tojavaobject_result = NULL;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checkindex(L, index)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, tojavaobject_protected);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 1, 1);
		tojavaobject_result = (jobject)lua_touserdata(L, -1);
		lua_pop(L, 1);
	}
	return tojavaobject_result;
}

/* lua_tonumber() */
JNIEXPORT jdouble JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1tonumber (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	lua_Number result = 0.0;
	if (checkindex(L, index)) {
		result = lua_tonumber(L, index);
	}
	return (jdouble) result;
}

/* lua_tonumberx() */
JNIEXPORT jobject JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1tonumberx (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	lua_Number result = 0.0;
	int isnum = 0;
	if (checkindex(L, index)) {
		result = lua_tonumberx(L, index, &isnum);
	}
	return isnum ? (*env)->CallStaticObjectMethod(env, double_class, valueof_double_id, (jdouble) result) : NULL;
}

/* lua_topointer() */
JNIEXPORT jlong JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1topointer (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	const void *result = NULL;
	if (checkindex(L, index)) {
		result = lua_topointer(L, index);
	}
	return (jlong) (uintptr_t) result;
}

/* lua_tostring() */
static int tostring_protected (lua_State *L) {
	lua_pushlightuserdata(L, (void*)lua_tostring(L, 1));
	return 1;
}
JNIEXPORT jstring JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1tostring (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	const char *tostring_result = NULL;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checkindex(L, index)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, tostring_protected);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 1, 1);
		tostring_result = (const char*)lua_touserdata(L, -1);
		lua_pop(L, 1);
	}
	return tostring_result ? (*env)->NewStringUTF(env, tostring_result) : NULL;
}

/* lua_type() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1type (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (!validindex(L, index)) {
		return LUA_TNONE;
	}
	return (jint) lua_type(L, index);
}

/* ---- Stack operations ---- */
/* lua_absindex() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1absindex (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	return (jint) lua_absindex(L, index);
}

/* lua_arith() */
static int arith_protected (lua_State *L) {
	lua_arith(L, lua_tointeger(L, 1));
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1arith (JNIEnv *env, jobject obj, jint op) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checknelems(L, op != LUA_OPUNM ? 2 : 1)) {
		lua_pushcfunction(L, arith_protected);
		lua_pushinteger(L, op);
		if (op != LUA_OPUNM) {
			lua_insert(L, -4);
			lua_insert(L, -4);
			JNLUA_PCALL(L, 3, 1);
		} else {
			lua_insert(L, -3);
			lua_insert(L, -3);
			JNLUA_PCALL(L, 2, 1);
		}
	}
}

/* lua_concat() */
static int concat_protected (lua_State *L) {
	lua_concat(L, lua_tointeger(L, 1));
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1concat (JNIEnv *env, jobject obj, jint n) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checkarg(n >= 0, "illegal count")
			&& checknelems(L, n)) {
		lua_pushcfunction(L, concat_protected);
		lua_pushinteger(L, n);
		lua_insert(L, -n - 2);
		lua_insert(L, -n - 2);
		JNLUA_PCALL(L, n + 1, 1);
	}
}

/* lua_copy() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1copy (JNIEnv *env, jobject obj, jint from_index, jint to_index) {
	lua_State *L = getluathread(env, obj);
	if (checkindex(L, from_index)
			&& checkindex(L, to_index)) {
		lua_copy(L, from_index, to_index);
	}
}

/* lua_gettop() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1gettop (JNIEnv *env, jobject obj) {
	lua_State *L = getluathread(env, obj);
	return (jint) lua_gettop(L);
}

/* lua_len() */
static int len_protected (lua_State *L) {
	lua_len(L, 1);
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1len (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checkindex(L, index)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, len_protected);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 1, 1);
	}
}

/* lua_insert() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1insert (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (checkrealindex(L, index)) {
		lua_insert(L, index);
	}
}

/* lua_pop() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1pop (JNIEnv *env, jobject obj, jint n) {
	lua_State *L = getluathread(env, obj);
	if (checkarg(n >= 0 && n <= lua_gettop(L), "illegal count")) {
		lua_pop(L, n);
	}
}

/* lua_pushvalue() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1pushvalue (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checkindex(L, index)) {
		lua_pushvalue(L, index);
	}
}

/* lua_remove() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1remove (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (checkrealindex(L, index)) {
		lua_remove(L, index);
	}
}

/* lua_replace() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1replace (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (checkindex(L, index)
			&& checknelems(L, 1)) {
		lua_replace(L, index);
	}
}

/* lua_settop() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1settop (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if ((index >= 0 && (index <= lua_gettop(L) || checkstack(L, index - lua_gettop(L))))
			|| (index < 0 && checkrealindex(L, index))) {
		lua_settop(L, index);
	}
}

/* ---- Table ---- */
/* lua_createtable() */
static int createtable_protected (lua_State *L) {
	lua_createtable(L, lua_tointeger(L, 1), lua_tointeger(L, 2));
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1createtable (JNIEnv *env, jobject obj, jint narr, jint nrec) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checkarg(narr >= 0, "illegal array count")
			&& checkarg(nrec >= 0, "illegal record count")) {
		lua_pushcfunction(L, createtable_protected);
		lua_pushinteger(L, narr);
		lua_pushinteger(L, nrec);
		JNLUA_PCALL(L, 2, 1);
	}
}

/* lua_getsubtable() */
static int getsubtable_protected (lua_State *L) {
	lua_pushboolean(L, luaL_getsubtable(L, 2, (const char*)lua_touserdata(L, 1)));
	return 2;
}
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1getsubtable (JNIEnv *env, jobject obj, jint index, jstring fname) {
	lua_State *L = getluathread(env, obj);
	const char *getsubtable_fname = NULL;
	jint getsubtable_result = 0;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checkindex(L, index)
			&& (getsubtable_fname = getstringchars(env, fname))) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, getsubtable_protected);
		lua_pushlightuserdata(L, (void*)getsubtable_fname);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 2, 2);
		getsubtable_result = (jint)lua_toboolean(L, -1);
		lua_pop(L, 1);
	}
	if (getsubtable_fname) {
		releasestringchars(env, fname, getsubtable_fname);
	}
	return getsubtable_result;
}

/* lua_getfield() */
static int getfield_protected (lua_State *L) {
	lua_getfield(L, 2, (const char*)lua_touserdata(L, 1));
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1getfield (JNIEnv *env, jobject obj, jint index, jstring k) {
	lua_State *L = getluathread(env, obj);
	const char *getfield_k = NULL;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)
			&& (getfield_k = getstringchars(env, k))) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, getfield_protected);
		lua_pushlightuserdata(L, (void*)getfield_k);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 2, 1);
	}
	if (getfield_k) {
		releasestringchars(env, k, getfield_k);
	}
}

/* lua_gettable() */
static int gettable_protected (lua_State *L) {
	lua_gettable(L, 1);
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1gettable (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, gettable_protected);
		lua_insert(L, -2);
		lua_pushvalue(L, index);
		lua_insert(L, -2);
		JNLUA_PCALL(L, 2, 1);
	}
}

/* lua_newtable() */
static int newtable_protected (lua_State *L) {
	lua_newtable(L);
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1newtable (JNIEnv *env, jobject obj) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)) {
		lua_pushcfunction(L, newtable_protected);
		JNLUA_PCALL(L, 0, 1);
	}
}

/* lua_next() */
static int next_protected (lua_State *L) {
	int next_result = lua_next(L, 1);
	lua_pushboolean(L, next_result);
	return next_result ? 3 : 1;
}
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1next (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	jint next_result = 0;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, next_protected);
		lua_insert(L, -2);
		lua_pushvalue(L, index);
		lua_insert(L, -2);
		JNLUA_PCALL(L, 2, LUA_MULTRET);
		next_result = (jint)lua_toboolean(L, -1);
		lua_pop(L, 1);
	}
	return next_result;
}

/* lua_rawget() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1rawget (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (checktype(L, index, LUA_TTABLE)) {
		lua_rawget(L, index);
	}
}

/* lua_rawgeti() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1rawgeti (JNIEnv *env, jobject obj, jint index, jint n) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)) {
		lua_rawgeti(L, index, n);
	}
}

/* lua_rawset() */
static int rawset_protected (lua_State *L) {
	lua_rawset(L, 1);
	return 0;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1rawset (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)
			&& checknelems(L, 2)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, rawset_protected);
		lua_insert(L, -3);
		lua_pushvalue(L, index);
		lua_insert(L, -3);
		JNLUA_PCALL(L, 3, 0);
	}
}

/* lua_rawseti() */
static int rawseti_protected (lua_State *L) {
	lua_rawseti(L, 2, lua_tointeger(L, 1));
	return 0;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1rawseti (JNIEnv *env, jobject obj, jint index, jint n) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, rawseti_protected);
		lua_insert(L, -2);
		lua_pushinteger(L, n);
		lua_insert(L, -2);
		lua_pushvalue(L, index);
		lua_insert(L, -2);
		JNLUA_PCALL(L, 3, 0);
	}
}

/* lua_settable() */
static int settable_protected (lua_State *L) {
	lua_settable(L, 1);
	return 0;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1settable (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)
			&& checknelems(L, 2)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, settable_protected);
		lua_insert(L, -3);
		lua_pushvalue(L, index);
		lua_insert(L, -3);
		JNLUA_PCALL(L, 3, 0);
	}
}

/* lua_setfield() */
static int setfield_protected (lua_State *L) {
	lua_setfield(L, 2, (const char*)lua_touserdata(L, 1));
	return 0;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1setfield (JNIEnv *env, jobject obj, jint index, jstring k) {
	lua_State *L = getluathread(env, obj);
	const char *setfield_k = NULL;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)
			&& (setfield_k = getstringchars(env, k))) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, setfield_protected);
		lua_insert(L, -2);
		lua_pushlightuserdata(L, (void*)setfield_k);
		lua_insert(L, -2);
		lua_pushvalue(L, index);
		lua_insert(L, -2);
		JNLUA_PCALL(L, 3, 0);
	}
	if (setfield_k) {
		releasestringchars(env, k, setfield_k);
	}
}

/* ---- Metatable ---- */
/* lua_getmetatable() */
JNIEXPORT int JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1getmetatable (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	int result = 0;
	if (lua_checkstack(L, JNLUA_MINSTACK)
			&& checkindex(L, index)) {
		result = lua_getmetatable(L, index);
	}
	return (jint) result;
}

/* lua_setmetatable() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1setmetatable (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	if (checkindex(L, index)
			&& checknelems(L, 1)
			&& checkarg(lua_type(L, -1) == LUA_TTABLE || lua_type(L, -1) == LUA_TNIL, "illegal type")) {
		lua_setmetatable(L, index);
	}
}

/* lua_getmetafield() */
static int getmetafield_protected (lua_State *L) {
	int getmetafield_result = luaL_getmetafield(L, 2, (const char*)lua_touserdata(L, 1));
	lua_pushboolean(L, getmetafield_result);
	return getmetafield_result ? 2 : 1;
}
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1getmetafield (JNIEnv *env, jobject obj, jint index, jstring k) {
	lua_State *L = getluathread(env, obj);
	const char *getmetafield_k = NULL;
	jint getmetafield_result = 0;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checkindex(L, index)
			&& (getmetafield_k = getstringchars(env, k))) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, getmetafield_protected);
		lua_pushlightuserdata(L, (void*)getmetafield_k);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 1, LUA_MULTRET);
		getmetafield_result = (jint)lua_toboolean(L, -1);
		lua_pop(L, 1);
	}
	if (getmetafield_k) {
		releasestringchars(env, k, getmetafield_k);
	}
	return getmetafield_result;
}

/* ---- Thread ---- */
/* lua_newthread() */
static int newthread_protected (lua_State *L) {
	lua_State *T;
	
	T = lua_newthread(L);
	lua_insert(L, 1);
	lua_xmove(L, T, 1);
	return 1;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1newthread (JNIEnv *env, jobject obj) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, -1, LUA_TFUNCTION)) {
		lua_pushcfunction(L, newthread_protected);
		lua_insert(L, -2);
		JNLUA_PCALL(L, 1, 1);
	}
}

/* lua_resume() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1resume (JNIEnv *env, jobject obj, jint index, jint nargs) {
	lua_State *L = getluathread(env, obj), *T;
	int status;
	int nresults = 0;
	if (checktype(L, index, LUA_TTHREAD)
			&& checkarg(nargs >= 0, "illegal argument count")
			&& checknelems(L, nargs + 1)) {
		T = lua_tothread(L, index);
		if (checkstack(T, nargs)) {
			lua_xmove(L, T, nargs);
			status = lua_resume(T, L, nargs);
			switch (status) {
			case LUA_OK:
			case LUA_YIELD:
				nresults = lua_gettop(T);
				if (checkstack(L, nresults)) {
					lua_xmove(T, L, nresults);
				}
				break;
			default:
				throw(L, status);
			}
		}
	}
	return (jint) nresults;
}

/* lua_status() */
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1status (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	int result = 0;
	if (checktype(L, index, LUA_TTHREAD)) {
		result = lua_status(lua_tothread(L, index));
	}
	return (jint) result;	
}

/* ---- Reference ---- */
/* lua_ref() */
static int ref_protected (lua_State *L) {
	lua_pushinteger(L, luaL_ref(L, 1));
	return 1;
}
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1ref (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	jint ref_result = 0;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, ref_protected);
		lua_insert(L, -2);
		lua_pushvalue(L, index);
		lua_insert(L, -2);
		JNLUA_PCALL(L, 2, 1);
		ref_result = lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	return ref_result;
}

/* lua_unref() */
static int unref_protected (lua_State *L) {
	luaL_unref(L, 2, lua_tointeger(L, 1));
	return 0;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1unref (JNIEnv *env, jobject obj, jint index, jint ref) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, unref_protected);
		lua_pushinteger(L, ref);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 2, 0);
	}
}

/* ---- Debug ---- */
/* lua_getstack() */
JNIEXPORT jobject JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1getstack (JNIEnv *env, jobject obj, jint level) {
	lua_State *L = getluathread(env, obj);
	lua_Debug *ar = NULL;
	jobject result = NULL;
	if (checkarg(level >= 0, "illegal level")) {
		ar = malloc(sizeof(lua_Debug));
		if (ar) {
			memset(ar, 0, sizeof(lua_Debug));
			if (lua_getstack(L, level, ar)) {
				result = (*env)->NewObject(env, luadebug_class, luadebug_init_id, (jlong) (uintptr_t) ar, JNI_TRUE);
			}
		}
	}
	if (!result) {
		free(ar);
	}
	return result;
}

/* lua_getinfo() */
static int getinfo_protected (lua_State *L) {
	lua_pushinteger(L, lua_getinfo(L, (const char*)lua_touserdata(L, 1), getluadebug(getthreadenv(), (jobject)lua_touserdata(L, 2))));
	return 0;
}
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1getinfo (JNIEnv *env, jobject obj, jstring what, jobject ar) {
	lua_State *L = getluathread(env, obj);
	const char *getinfo_what = NULL;
	int getinfo_result = 0;
	if (checkstack(L, JNLUA_MINSTACK)
			&& (getinfo_what = getstringchars(env, what))
			&& checknotnull(ar)) {
		lua_pushcfunction(L, getinfo_protected);
		lua_pushlightuserdata(L, (void*)getinfo_what);
		lua_pushlightuserdata(L, (void*)ar);
		JNLUA_PCALL(L, 2, 1);
		getinfo_result = lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	if (getinfo_what) {
		releasestringchars(env, what, getinfo_what);
	}
	return getinfo_result;
}

/* ---- Optimization ---- */
/* lua_tablesize() */
static int tablesize_protected (lua_State *L) {
	int count = 0;
	
	lua_pushnil(L);
	while (lua_next(L, -2)) {
		lua_pop(L, 1);
		count++;
	}
	lua_pushinteger(L, count);
	return 1;
}
JNIEXPORT jint JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1tablesize (JNIEnv *env, jobject obj, jint index) {
	lua_State *L = getluathread(env, obj);
	jint tablesize_result;
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, tablesize_protected);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 1, 1);
		tablesize_result = (jint)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	return tablesize_result;
}

/* lua_tablemove() */
static int tablemove_protected (lua_State *L) {
	int from = lua_tointeger(L, 1), to = lua_tointeger(L, 2);
	int count = lua_tointeger(L, 3), i;
	
	if (from < to) {
		for (i = count - 1; i >= 0; i--) {
			lua_rawgeti(L, 4, from + i);
			lua_rawseti(L, 4, to + i);
		}
	} else if (from > to) {
		for (i = 0; i < count; i++) { 
			lua_rawgeti(L, 4, from + i);
			lua_rawseti(L, 4, to + i);
		}
	}
	return 0;
}
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_lua_1tablemove (JNIEnv *env, jobject obj, jint index, jint from, jint to, jint count) {
	lua_State *L = getluathread(env, obj);
	if (checkstack(L, JNLUA_MINSTACK)
			&& checktype(L, index, LUA_TTABLE)
			&& checkarg(count >= 0, "illegal count")) {
		index = lua_absindex(L, index);
		lua_pushcfunction(L, tablemove_protected);
		lua_pushinteger(L, from);
		lua_pushinteger(L, to);
		lua_pushinteger(L, count);
		lua_pushvalue(L, index);
		JNLUA_PCALL(L, 4, 0);
	}
}

/* ---- Debug structure ---- */
/* lua_debugfree() */
JNIEXPORT void JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_00024LuaDebug_lua_1debugfree (JNIEnv *env, jobject obj) {
	lua_Debug *ar = getluadebug(env, obj);
	setluadebug(env, obj, NULL);
	free(ar);
}

/* lua_debugname() */
JNIEXPORT jstring JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_00024LuaDebug_lua_1debugname (JNIEnv *env, jobject obj) {
	lua_Debug *ar = getluadebug(env, obj);
	return ar != NULL && ar->name != NULL ? (*env)->NewStringUTF(env, ar->name) : NULL;
}

/* lua_debugnamewhat() */
JNIEXPORT jstring JNICALL Java_li_cil_repack_com_naef_jnlua_LuaState_00024LuaDebug_lua_1debugnamewhat (JNIEnv *env, jobject obj) {
	lua_Debug *ar = getluadebug(env, obj);
	return ar != NULL && ar->namewhat != NULL ? (*env)->NewStringUTF(env, ar->namewhat) : NULL;
}

/* ---- JNI ---- */
/* Handles the loading of this library. */
JNIEXPORT jint JNICALL JNI_OnLoad (JavaVM *vm, void *reserved) {
	JNIEnv *env;
	
	/* Get environment */
	if ((*vm)->GetEnv(vm, (void **) &env, JNLUA_JNIVERSION) != JNI_OK) {
		return JNLUA_JNIVERSION;
	}

	/* Lookup and pin classes, fields and methods */
	if (!(luastate_class = referenceclass(env, "li/cil/repack/com/naef/jnlua/LuaState"))
			|| !(luastate_id = (*env)->GetFieldID(env, luastate_class, "luaState", "J"))
			|| !(luathread_id = (*env)->GetFieldID(env, luastate_class, "luaThread", "J"))
			|| !(luamemorytotal_id = (*env)->GetFieldID(env, luastate_class, "luaMemoryTotal", "I"))
			|| !(luamemoryused_id = (*env)->GetFieldID(env, luastate_class, "luaMemoryUsed", "I"))
			|| !(yield_id = (*env)->GetFieldID(env, luastate_class, "yield", "Z"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luadebug_class = referenceclass(env, "li/cil/repack/com/naef/jnlua/LuaState$LuaDebug"))
			|| !(luadebug_init_id = (*env)->GetMethodID(env, luadebug_class, "<init>", "(JZ)V"))
			|| !(luadebug_field_id = (*env)->GetFieldID(env, luadebug_class, "luaDebug", "J"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(javafunction_interface = referenceclass(env, "li/cil/repack/com/naef/jnlua/JavaFunction"))
			|| !(invoke_id = (*env)->GetMethodID(env, javafunction_interface, "invoke", "(Lli/cil/repack/com/naef/jnlua/LuaState;)I"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luaruntimeexception_class = referenceclass(env, "li/cil/repack/com/naef/jnlua/LuaRuntimeException"))
			|| !(luaruntimeexception_id = (*env)->GetMethodID(env, luaruntimeexception_class, "<init>", "(Ljava/lang/String;)V"))
			|| !(setluaerror_id = (*env)->GetMethodID(env, luaruntimeexception_class, "setLuaError", "(Lli/cil/repack/com/naef/jnlua/LuaError;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luasyntaxexception_class = referenceclass(env, "li/cil/repack/com/naef/jnlua/LuaSyntaxException"))
			|| !(luasyntaxexception_id = (*env)->GetMethodID(env, luasyntaxexception_class, "<init>", "(Ljava/lang/String;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luamemoryallocationexception_class = referenceclass(env, "li/cil/repack/com/naef/jnlua/LuaMemoryAllocationException"))
			|| !(luamemoryallocationexception_id = (*env)->GetMethodID(env, luamemoryallocationexception_class, "<init>", "(Ljava/lang/String;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luagcmetamethodexception_class = referenceclass(env, "li/cil/repack/com/naef/jnlua/LuaGcMetamethodException"))
			|| !(luagcmetamethodexception_id = (*env)->GetMethodID(env, luagcmetamethodexception_class, "<init>", "(Ljava/lang/String;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luamessagehandlerexception_class = referenceclass(env, "li/cil/repack/com/naef/jnlua/LuaMessageHandlerException"))
			|| !(luamessagehandlerexception_id = (*env)->GetMethodID(env, luamessagehandlerexception_class, "<init>", "(Ljava/lang/String;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luastacktraceelement_class = referenceclass(env, "li/cil/repack/com/naef/jnlua/LuaStackTraceElement"))
			|| !(luastacktraceelement_id = (*env)->GetMethodID(env, luastacktraceelement_class, "<init>", "(Ljava/lang/String;Ljava/lang/String;I)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luaerror_class = referenceclass(env, "li/cil/repack/com/naef/jnlua/LuaError"))
			|| !(luaerror_id = (*env)->GetMethodID(env, luaerror_class, "<init>", "(Ljava/lang/String;Ljava/lang/Throwable;)V"))
			|| !(setluastacktrace_id = (*env)->GetMethodID(env, luaerror_class, "setLuaStackTrace", "([Lli/cil/repack/com/naef/jnlua/LuaStackTraceElement;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(nullpointerexception_class = referenceclass(env, "java/lang/NullPointerException"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(illegalargumentexception_class = referenceclass(env, "java/lang/IllegalArgumentException"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(illegalstateexception_class = referenceclass(env, "java/lang/IllegalStateException"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(error_class = referenceclass(env, "java/lang/Error"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(integer_class = referenceclass(env, "java/lang/Integer"))
			|| !(valueof_integer_id = (*env)->GetStaticMethodID(env, integer_class, "valueOf", "(I)Ljava/lang/Integer;"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(double_class = referenceclass(env, "java/lang/Double"))
			|| !(valueof_double_id = (*env)->GetStaticMethodID(env, double_class, "valueOf", "(D)Ljava/lang/Double;"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(inputstream_class = referenceclass(env, "java/io/InputStream"))
			|| !(read_id = (*env)->GetMethodID(env, inputstream_class, "read", "([B)I"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(outputstream_class = referenceclass(env, "java/io/OutputStream"))
			|| !(write_id = (*env)->GetMethodID(env, outputstream_class, "write", "([BII)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(ioexception_class = referenceclass(env, "java/io/IOException"))) {
		return JNLUA_JNIVERSION;
	}

	/* OK */
	initialized = 1;
	java_vm = vm;
	return JNLUA_JNIVERSION;
}

/* Handles the unloading of this library. */
JNIEXPORT void JNICALL JNI_OnUnload (JavaVM *vm, void *reserved) {
	JNIEnv *env;
	
	/* Get environment */
	if ((*vm)->GetEnv(vm, (void **) &env, JNLUA_JNIVERSION) != JNI_OK) {
		return;
	}
	
	/* Free classes */
	if (luastate_class) {
		(*env)->DeleteGlobalRef(env, luastate_class);
	}
	if (javafunction_interface) {
		(*env)->DeleteGlobalRef(env, javafunction_interface);
	}
	if (luaruntimeexception_class) {
		(*env)->DeleteGlobalRef(env, luaruntimeexception_class);
	}
	if (luasyntaxexception_class) {
		(*env)->DeleteGlobalRef(env, luasyntaxexception_class);
	}
	if (luamemoryallocationexception_class) {
		(*env)->DeleteGlobalRef(env, luamemoryallocationexception_class);
	}
	if (luagcmetamethodexception_class) {
		(*env)->DeleteGlobalRef(env, luagcmetamethodexception_class);
	}
	if (luamessagehandlerexception_class) {
		(*env)->DeleteGlobalRef(env, luamessagehandlerexception_class);
	}
	if (luastacktraceelement_class) {
		(*env)->DeleteGlobalRef(env, luastacktraceelement_class);
	}
	if (luaerror_class) {
		(*env)->DeleteGlobalRef(env, luaerror_class);
	}
	if (nullpointerexception_class) {
		(*env)->DeleteGlobalRef(env, nullpointerexception_class);
	}
	if (illegalargumentexception_class) {
		(*env)->DeleteGlobalRef(env, illegalargumentexception_class);
	}
	if (illegalstateexception_class) {
		(*env)->DeleteGlobalRef(env, illegalstateexception_class);
	}
	if (error_class) {
		(*env)->DeleteGlobalRef(env, error_class);
	}
	if (integer_class) {
		(*env)->DeleteGlobalRef(env, integer_class);
	}
	if (double_class) {
		(*env)->DeleteGlobalRef(env, double_class);
	}
	if (inputstream_class) {
		(*env)->DeleteGlobalRef(env, inputstream_class);
	}
	if (outputstream_class) {
		(*env)->DeleteGlobalRef(env, outputstream_class);
	}
	if (ioexception_class) {
		(*env)->DeleteGlobalRef(env, ioexception_class);
	}

	java_vm = NULL;
}

/* ---- JNI helpers ---- */
/* Finds a class and returns a new JNI global reference to it. */
static jclass referenceclass (JNIEnv *env, const char *className) {
	jclass clazz;
	
	clazz = (*env)->FindClass(env, className);
	if (!clazz) {
		return NULL;
	}
	return (*env)->NewGlobalRef(env, clazz);
}

/* Return a new JNI byte array. */
static jbyteArray newbytearray (JNIEnv *env, jsize length) {
	jbyteArray array;
	
	array = (*env)->NewByteArray(env, length);
	if (!check(array != NULL, luamemoryallocationexception_class, "JNI error: NewByteArray() failed")) {
		return NULL;
	}
	return array;
}

/* Returns the  UTF chars of a string. */
static const char *getstringchars (JNIEnv *env, jstring string) {
	const char *utf;

	if (!checknotnull(string)) {
		return NULL;
	}
	utf = (*env)->GetStringUTFChars(env, string, NULL);
	if (!check(utf != NULL, luamemoryallocationexception_class, "JNI error: GetStringUTFChars() failed")) {
		return NULL;
	}
	return utf;
}

/* Releaes the UTF chars of a string. */
static void releasestringchars (JNIEnv *env, jstring string, const char *chars) {
	(*env)->ReleaseStringUTFChars(env, string, chars);
}

/* ---- Java state operations ---- */
/* Returns the Lua state from the Java state. */
static lua_State *getluastate (JNIEnv *env, jobject javastate) {
	return (lua_State *) (uintptr_t) (*env)->GetLongField(env, javastate, luastate_id);
}

/* Sets the Lua state in the Java state. */
static void setluastate (JNIEnv *env, jobject javastate, lua_State *L) {
	(*env)->SetLongField(env, javastate, luastate_id, (jlong) (uintptr_t) L);
}

/* Returns the Lua thread from the Java state. */
static lua_State *getluathread (JNIEnv *env, jobject javastate) {
	return (lua_State *) (uintptr_t) (*env)->GetLongField(env, javastate, luathread_id);
}

/* Sets the Lua state in the Java state. */
static void setluathread (JNIEnv *env, jobject javastate, lua_State *L) {
	(*env)->SetLongField(env, javastate, luathread_id, (jlong) (uintptr_t) L);
}

/* Gets the amount of ram available and used for and by the current Lua state. */
static void getluamemory (JNIEnv *env, jobject obj, jint *total, jint *used) {
	*total = (*env)->GetIntField(env, obj, luamemorytotal_id);
	*used = (*env)->GetIntField(env, obj, luamemoryused_id);
}
/* Sets the amount of ram used by the current Lua state (called by allocator). */
static void setluamemory (JNIEnv *env, jobject obj, jint used) {
	(*env)->SetIntField(env, obj, luamemoryused_id, used);
}

/* Returns the yield flag from the Java state */
static int getyield (JNIEnv *env, jobject javastate) {
	return (int) (*env)->GetBooleanField(env, javastate, yield_id);
}

/* Sets the yield flag in the Java state */
static void setyield (JNIEnv *env, jobject javastate, int yield) {
	(*env)->SetBooleanField(env, javastate, yield_id, (jboolean) yield);
}

/* Returns the Lua debug structure in a Java debug object. */
static lua_Debug *getluadebug (JNIEnv *env, jobject javadebug) {
	return (lua_Debug *) (uintptr_t) (*env)->GetLongField(env, javadebug, luadebug_field_id);
}

/* Sets the Lua debug structure in a Java debug object. */
static void setluadebug (JNIEnv *env, jobject javadebug, lua_Debug *ar) {
	(*env)->SetLongField(env, javadebug, luadebug_field_id, (jlong) (uintptr_t) ar);
}

static JNIEnv *getthreadenv() {
	JNIEnv *env;
	if (java_vm && (*java_vm)->GetEnv(java_vm, (void**) &env, JNLUA_JNIVERSION) == JNI_OK) {
		return env;
	}
	return NULL;
}

/* ---- Checks ---- */
/* Returns whether an index is valid. */
static int validindex (lua_State *L, int index) {
	int top;
	
	top = lua_gettop(L);
	if (index <= 0) {
		if (index > LUA_REGISTRYINDEX) {
			index = top + index + 1;
		} else {
			switch (index) {
			case LUA_REGISTRYINDEX:
				return 1;
			default:
				return 0; /* C upvalue access not needed, don't even validate */
			}
		}
	}
	return index >= 1 && index <= top;
}

/* Checks stack space. */
static int checkstack (lua_State *L, int space) {
	return check(lua_checkstack(L, space), illegalstateexception_class, "stack overflow");
}

/* Checks if an index is valid. */
static int checkindex (lua_State *L, int index) {
	return checkarg(validindex(L, index), "illegal index");
}
	
/* Checks if an index is valid, ignoring pseudo indexes. */
static int checkrealindex (lua_State *L, int index) {
	int top;
	
	top = lua_gettop(L);
	if (index <= 0) {
		index = top + index + 1;
	}
	return checkarg(index >= 1 && index <= top, "illegal index");
}

/* Checks the type of a stack value. */
static int checktype (lua_State *L, int index, int type) {
	return checkindex(L, index)
			&& checkarg(lua_type(L, index) == type, "illegal type");
}
	
/* Checks that there are at least n values on the stack. */
static int checknelems (lua_State *L, int n) {
	return checkstate(lua_gettop(L) >= n, "stack underflow");
}

/* Checks an argument for not-null. */ 
static int checknotnull (void *object) {
	return check(object != NULL, nullpointerexception_class, "null");
}

/* Checks an argument condition. */
static int checkarg (int cond, const char *msg) {
	return check(cond, illegalargumentexception_class, msg);
}

/* Checks a state condition. */
static int checkstate (int cond, const char *msg) {
	return check(cond, illegalstateexception_class, msg);
}

/* Checks a condition. */
static int check (int cond, jthrowable throwable_class, const char *msg) {
	if (cond) {
		return 1;
	} else {
		JNIEnv *thread_env = getthreadenv();
		(*thread_env)->ThrowNew(thread_env, throwable_class, msg);
	}
	return 0;
}

/* ---- Java objects and functions ---- */
/* Pushes a Java object on the stack. */
static void pushjavaobject (lua_State *L, jobject object) {
	JNIEnv *thread_env = getthreadenv();
	jobject *user_data;
	
	user_data = (jobject *) lua_newuserdata(L, sizeof(jobject));
	luaL_getmetatable(L, JNLUA_OBJECT);
	*user_data = (*thread_env)->NewGlobalRef(thread_env, object);
	if (!*user_data) {
		lua_pushliteral(L, "JNI error: NewGlobalRef() failed pushing Java object");
		lua_error(L);
	}
	lua_setmetatable(L, -2);
}
	
/* Returns the Java object at the specified index, or NULL if such an object is unobtainable. */
static jobject tojavaobject (lua_State *L, int index, jclass class) {
	int result;
	jobject object;

	if (!lua_isuserdata(L, index)) {
		return NULL;
	}
	if (!lua_getmetatable(L, index)) {
		return NULL;
	}
	luaL_getmetatable(L, JNLUA_OBJECT);
	result = lua_rawequal(L, -1, -2);
	lua_pop(L, 2);
	if (!result) {
		return NULL;
	}
	object = *(jobject *) lua_touserdata(L, index);
	if (class) {
		JNIEnv *thread_env = getthreadenv();
		if (!(*thread_env)->IsInstanceOf(thread_env, object, class)) {
			return NULL;
		}
	}
	return object;
}

/* Returns a Java string for a value on the stack. */
static jstring tostring (lua_State *L, int index) {
	JNIEnv *thread_env = getthreadenv();
	jstring string;

	string = (*thread_env)->NewStringUTF(thread_env, luaL_tolstring(L, index, NULL));
	lua_pop(L, 1);
	return string;
}

/* Finalizes Java objects. */
static int gcjavaobject (lua_State *L) {
	JNIEnv *thread_env = getthreadenv();
	jobject obj;

	if (!thread_env) {
		/* Environment has been cleared as the Java VM was destroyed. Nothing to do. */
		return 0;
	}
	obj = *(jobject *) lua_touserdata(L, 1);
	if (lua_toboolean(L, lua_upvalueindex(1))) {
		(*thread_env)->DeleteWeakGlobalRef(thread_env, obj);
	} else {
		(*thread_env)->DeleteGlobalRef(thread_env, obj);
	}
	return 0;
}

/* Calls a Java function. If an exception is reported, store it as the cause for later use. */
static int calljavafunction (lua_State *L) {
	JNIEnv *thread_env = getthreadenv();
	jobject javastate, javafunction;
	lua_State *T;
	int nresults;
	jthrowable throwable;
	jstring where;
	jobject luaerror;
	
	/* Get Java state. */
	lua_getfield(L, LUA_REGISTRYINDEX, JNLUA_JAVASTATE);
	if (!lua_isuserdata(L, -1)) {
		/* Java state has been cleared as the Java VM was destroyed. Cannot call. */
		lua_pushliteral(L, "no Java state");
		return lua_error(L);
	}
	javastate = *(jobject *) lua_touserdata(L, -1);
	lua_pop(L, 1);
	
	/* Get Java function object. */
	lua_pushvalue(L, lua_upvalueindex(1));
	javafunction = tojavaobject(L, -1, javafunction_interface);
	lua_pop(L, 1);
	if (!javafunction) {
		/* Function was cleared from outside JNLua code. */
		lua_pushliteral(L, "no Java function");
		return lua_error(L);
	}
	
	/* Perform the call, handling coroutine situations. */
	setyield(thread_env, javastate, JNI_FALSE);
	T = getluathread(thread_env, javastate);
	if (T == L) {
		nresults = (*thread_env)->CallIntMethod(thread_env, javafunction, invoke_id, javastate);
	} else {
		setluathread(thread_env, javastate, L);
		nresults = (*thread_env)->CallIntMethod(thread_env, javafunction, invoke_id, javastate);
	}
	
	/* Handle exception */
	throwable = (*thread_env)->ExceptionOccurred(thread_env);
	if (throwable) {
		(*thread_env)->ExceptionClear(thread_env);
		setluathread(thread_env, javastate, T);
		/* Push exception & clear */
		luaL_where(L, 1);
		where = tostring(L, -1);
		luaerror = (*thread_env)->NewObject(thread_env, luaerror_class, luaerror_id, where, throwable);
		if (luaerror) {
			pushjavaobject(L, luaerror);
		} else {
			lua_pushliteral(L, "JNI error: NewObject() failed creating Lua error");
		}
		
		/* Error out */
		return lua_error(L);
	}
	else {
		setluathread(thread_env, javastate, T);
	}
	
	/* Handle yield */
	if (getyield(thread_env, javastate)) {
		if (nresults < 0 || nresults > lua_gettop(L)) {
			lua_pushliteral(L, "illegal return count");
			return lua_error(L);
		}
		if (L == getluastate(thread_env, javastate)) {
			lua_pushliteral(L, "not in a thread");
			return lua_error(L);
		}
		return lua_yield(L, nresults);
	}
	
	return nresults;
}

/* Handles Lua errors. */
static int messagehandler (lua_State *L) {
	JNIEnv *thread_env = getthreadenv();
	int level, count;
	lua_Debug ar;
	jobjectArray luastacktrace;
	jstring name, source;
	jobject luastacktraceelement;
	jobject luaerror;
	jstring message;

	/* Count relevant stack frames */
	level = 1;
	count = 0;
	while (lua_getstack(L, level, &ar)) {
		lua_getinfo(L, "nSl", &ar);
		if (isrelevant(&ar)) {
			count++;
		}
		level++;
	}
	
	/* Create Lua stack trace as a Java LuaStackTraceElement[] */
	luastacktrace = (*thread_env)->NewObjectArray(thread_env, count, luastacktraceelement_class, NULL);
	if (!luastacktrace) {
		return 1;
	}
	level = 1;
	count = 0;
	while (lua_getstack(L, level, &ar)) {
		lua_getinfo(L, "nSl", &ar);
		if (isrelevant(&ar)) {
			name = ar.name ? (*thread_env)->NewStringUTF(thread_env, ar.name) : NULL;
			source = ar.source ? (*thread_env)->NewStringUTF(thread_env, ar.source) : NULL;
			luastacktraceelement = (*thread_env)->NewObject(thread_env, luastacktraceelement_class,	luastacktraceelement_id, name, source, ar.currentline);
			if (!luastacktraceelement) {
				return 1;
			}
			(*thread_env)->SetObjectArrayElement(thread_env, luastacktrace, count, luastacktraceelement);
			if ((*thread_env)->ExceptionCheck(thread_env)) {
				return 1;
			}
			count++;
		}
		level++;
	}
	
	/* Get or create the error object  */
	luaerror = tojavaobject(L, -1, luaerror_class);
	if (!luaerror) {
		message = tostring(L, -1);
		if (!(luaerror = (*thread_env)->NewObject(thread_env, luaerror_class, luaerror_id, message, NULL))) {
			return 1;
		}
	}
	(*thread_env)->CallVoidMethod(thread_env, luaerror, setluastacktrace_id, luastacktrace);
	
	/* Replace error */
	pushjavaobject(L, luaerror);
	return 1;
}

/* Processes a Lua activation record and returns whether it is relevant. */
static int isrelevant (lua_Debug *ar) {
	if (ar->name && strlen(ar->name) == 0) {
		ar->name = NULL;
	}
	if (ar->what && strcmp(ar->what, "C") == 0) {
		ar->source = NULL;
	}
	if (ar->source) {
		if (*ar->source == '=' || *ar->source == '@') {
			ar->source++;
		}
	}
	return ar->name || ar->source;
}

/* Handles Lua errors by throwing a Java exception. */
static int throw_protected (lua_State *L) {
	JNIEnv *thread_env = getthreadenv();
	jclass class;
	jmethodID id;
	jthrowable throwable;
	jobject luaerror;
	int throw_status = lua_tointeger(L, 2);
	lua_pop(L, 1);
	
	/* Determine the type of exception to throw. */
	switch (throw_status) {
	case LUA_ERRRUN:
		class = luaruntimeexception_class;
		id = luaruntimeexception_id;
		break;
	case LUA_ERRSYNTAX:
		class = luasyntaxexception_class;
		id = luasyntaxexception_id;
		break;
	case LUA_ERRMEM:
		class = luamemoryallocationexception_class;
		id = luamemoryallocationexception_id;
		break;
	case LUA_ERRGCMM:
		class = luagcmetamethodexception_class;
		id = luagcmetamethodexception_id;
		break;
	case LUA_ERRERR:
		class = luamessagehandlerexception_class;
		id = luamessagehandlerexception_id;
		break;
	default:
		lua_pushfstring(L, "unknown Lua status %d", throw_status);
		return lua_error(L);
	}
	
	/* Create exception */
	throwable = (*thread_env)->NewObject(thread_env, class, id, tostring(L, 1));
	if (!throwable) {
		lua_pushliteral(L, "JNI error: NewObject() failed creating throwable");
		return lua_error(L);
	}
		
	/* Set the Lua error, if any. */
	luaerror = tojavaobject(L, 1, luaerror_class);
	if (luaerror && class == luaruntimeexception_class) {
		(*thread_env)->CallVoidMethod(thread_env, throwable, setluaerror_id, luaerror);
	}
	
	/* Throw */
	if ((*thread_env)->Throw(thread_env, throwable) < 0) {
		lua_pushliteral(L, "JNI error: Throw() failed");
		return lua_error(L);
	}
	
	return 0;
}
static void throw (lua_State *L, int status) {
	JNIEnv *thread_env = getthreadenv();
	const char *message;
	
	if (checkstack(L, JNLUA_MINSTACK)) {
		lua_pushcfunction(L, throw_protected);
		lua_insert(L, -2);
		lua_pushinteger(L, status);
		if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
			message = lua_tostring(L, -1);
			(*thread_env)->ThrowNew(thread_env, error_class, message ? message : "error throwing Lua exception");
		}
	}
}

/* ---- Stream adapters ---- */
/* Lua reader for Java input streams. */
static const char *readhandler (lua_State *L, void *ud, size_t *size) {
	JNIEnv *thread_env = getthreadenv();
	Stream *stream;
	int read;

	stream = (Stream *) ud;
	read = (*thread_env)->CallIntMethod(thread_env, stream->stream, read_id, stream->byte_array);
	if ((*thread_env)->ExceptionCheck(thread_env)) {
		return NULL;
	}
	if (read == -1) {
		return NULL;
	}
	if (stream->bytes && stream->is_copy) {
		(*thread_env)->ReleaseByteArrayElements(thread_env, stream->byte_array, stream->bytes, JNI_ABORT);
		stream->bytes = NULL;
	}
	if (!stream->bytes) {
		stream->bytes = (*thread_env)->GetByteArrayElements(thread_env, stream->byte_array, &stream->is_copy);
		if (!stream->bytes) {
			(*thread_env)->ThrowNew(thread_env, ioexception_class, "JNI error: GetByteArrayElements() failed accessing IO buffer");
			return NULL;
		}
	}
	*size = (size_t) read;
	return (const char *) stream->bytes;
}

/* Lua writer for Java output streams. */
static int writehandler (lua_State *L, const void *data, size_t size, void *ud) {
	JNIEnv *thread_env = getthreadenv();
	Stream *stream;

	stream = (Stream *) ud;
	if (!stream->bytes) {
		stream->bytes = (*thread_env)->GetByteArrayElements(thread_env, stream->byte_array, &stream->is_copy);
		if (!stream->bytes) {
			(*thread_env)->ThrowNew(thread_env, ioexception_class, "JNI error: GetByteArrayElements() failed accessing IO buffer");
			return 1;
		}
	}
	memcpy(stream->bytes, data, size);
	if (stream->is_copy) {
		(*thread_env)->ReleaseByteArrayElements(thread_env, stream->byte_array, stream->bytes, JNI_COMMIT);
	}
	(*thread_env)->CallVoidMethod(thread_env, stream->stream, write_id, stream->byte_array, 0, size);
	if ((*thread_env)->ExceptionCheck(thread_env)) {
		return 1;
	}
	return 0;
}
