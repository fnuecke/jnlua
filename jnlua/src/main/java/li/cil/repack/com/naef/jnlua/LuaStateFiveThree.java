package li.cil.repack.com.naef.jnlua;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class LuaStateFiveThree extends LuaState {
	public static final int REGISTRYINDEX;

	public static final String LUA_VERSION;

	static {
		NativeSupport.getInstance().getLoader().load();
		REGISTRYINDEX = lua_registryindex();
		LUA_VERSION = lua_version();
	}

	@Override
	protected int REGISTRYINDEX() {
		return REGISTRYINDEX;
	}

	@Override
	protected String LUA_VERSION() {
		return LUA_VERSION;
	}

	public LuaStateFiveThree() {
	}

	public LuaStateFiveThree(int memory) {
		super(memory);
	}

	public static native int lua_registryindex();

	public static native String lua_version();

	@Override
	protected native void lua_newstate(int apiversion, long luaState);

	@Override
	protected native void lua_close(boolean ownState);

	@Override
	protected native int lua_gc(int what, int data);

	@Override
	protected native void lua_openlib(int lib);

	@Override
	protected native void lua_load(InputStream inputStream, String chunkname,
			String mode) throws IOException;

	@Override
	protected native void lua_dump(OutputStream outputStream) throws IOException;

	@Override
	protected native void lua_pcall(int nargs, int nresults);

	@Override
	protected native void lua_getglobal(String name);

	@Override
	protected native void lua_setglobal(String name);

	@Override
	protected native void lua_pushboolean(int b);

	@Override
	protected native void lua_pushbytearray(byte[] b);
	
	@Override
	protected native void lua_pushinteger(int n);

	@Override
	protected native void lua_pushjavafunction(JavaFunction f);

	@Override
	protected native void lua_pushjavaobject(Object object);

	@Override
	protected native void lua_pushnil();

	@Override
	protected native void lua_pushnumber(double n);

	@Override
	protected native void lua_pushstring(String s);

	@Override
	protected native int lua_isboolean(int index);

	@Override
	protected native int lua_iscfunction(int index);

	@Override
	protected native int lua_isfunction(int index);

	@Override
	protected native int lua_isjavafunction(int index);

	@Override
	protected native int lua_isjavaobject(int index);

	@Override
	protected native int lua_isnil(int index);

	@Override
	protected native int lua_isnone(int index);

	@Override
	protected native int lua_isnoneornil(int index);

	@Override
	protected native int lua_isnumber(int index);

	@Override
	protected native int lua_isstring(int index);

	@Override
	protected native int lua_istable(int index);

	@Override
	protected native int lua_isthread(int index);

	@Override
	protected native int lua_compare(int index1, int index2, int operator);

	@Override
	protected native int lua_rawequal(int index1, int index2);

	@Override
	protected native int lua_rawlen(int index);

	@Override
	protected native int lua_toboolean(int index);

	@Override
	protected native byte[] lua_tobytearray(int index);
	
	@Override
	protected native int lua_tointeger(int index);

	@Override
	protected native Integer lua_tointegerx(int index);

	@Override
	protected native JavaFunction lua_tojavafunction(int index);

	@Override
	protected native Object lua_tojavaobject(int index);

	@Override
	protected native double lua_tonumber(int index);

	@Override
	protected native Double lua_tonumberx(int index);

	@Override
	protected native long lua_topointer(int index);

	@Override
	protected native String lua_tostring(int index);

	@Override
	protected native int lua_type(int index);

	@Override
	protected native int lua_absindex(int index);

	@Override
	protected native int lua_arith(int operator);

	@Override
	protected native void lua_concat(int n);

	@Override
	protected native int lua_copy(int fromIndex, int toIndex);

	@Override
	protected native int lua_gettop();

	@Override
	protected native void lua_len(int index);

	@Override
	protected native void lua_insert(int index);

	@Override
	protected native void lua_pop(int n);

	@Override
	protected native void lua_pushvalue(int index);

	@Override
	protected native void lua_remove(int index);

	@Override
	protected native void lua_replace(int index);

	@Override
	protected native void lua_settop(int index);

	@Override
	protected native void lua_createtable(int narr, int nrec);

	@Override
	protected native int lua_getsubtable(int idx, String fname);

	@Override
	protected native void lua_gettable(int index);

	@Override
	protected native void lua_getfield(int index, String k);

	@Override
	protected native void lua_newtable();

	@Override
	protected native int lua_next(int index);

	@Override
	protected native void lua_rawget(int index);

	@Override
	protected native void lua_rawgeti(int index, int n);

	@Override
	protected native void lua_rawset(int index);

	@Override
	protected native void lua_rawseti(int index, int n);

	@Override
	protected native void lua_settable(int index);

	@Override
	protected native void lua_setfield(int index, String k);

	@Override
	protected native int lua_getmetatable(int index);

	@Override
	protected native void lua_setmetatable(int index);

	@Override
	protected native int lua_getmetafield(int index, String k);

	@Override
	protected native void lua_newthread();

	@Override
	protected native int lua_resume(int index, int nargs);

	@Override
	protected native int lua_status(int index);

	@Override
	protected native int lua_ref(int index);

	@Override
	protected native void lua_unref(int index, int ref);

	@Override
	protected native LuaState.LuaDebug lua_getstack(int level);

	@Override
	protected native int lua_getinfo(String what, LuaState.LuaDebug ar);

	@Override
	protected native int lua_tablesize(int index);

	@Override
	protected native void lua_tablemove(int index, int from, int to, int count);

	protected static class LuaDebug extends LuaState.LuaDebug {
		protected LuaDebug(long luaDebug, boolean ownDebug) {
			super(luaDebug, ownDebug);
		}

		@Override
		protected native void lua_debugfree();

		@Override
		protected native String lua_debugname();

		@Override
		protected native String lua_debugnamewhat();
	}
}
