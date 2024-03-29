#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <ctype.h>
#include <algorithm>
#include <vector>

using std::min;
using std::max;

#ifdef __linux
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

extern "C" {
	#include <lua.h>
	#include <lauxlib.h>
	#include <lualib.h>
	#include <lstate.h>
}

#include "PsxCommon.h"
#ifdef WIN32
#include "Win32/Win32.h"
#include "Win32/resource.h"
#endif
#include "LuaEngine.h"

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#ifndef inline
#define inline __inline
#endif

static void(*info_print)(int uid, const char* str);
static void(*info_onstart)(int uid);
static void(*info_onstop)(int uid);
static int info_uid;
#ifdef WIN32
extern HWND LuaConsoleHWnd;
extern INT_PTR CALLBACK DlgLuaScriptDialog(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
extern void PrintToWindowConsole(int hDlgAsInt, const char* str);
extern void WinLuaOnStart(int hDlgAsInt);
extern void WinLuaOnStop(int hDlgAsInt);
#endif

static lua_State *LUA;

// Screen
static uint8 *XBuf;
static int iScreenWidth,iScreenHeight;

// Current working directory of the script
static char luaCWD [_MAX_PATH] = {0};

// Are we running any code right now?
static char *luaScriptName = NULL;

// Are we running any code right now?
static int luaRunning = FALSE;

// True at the frame boundary, false otherwise.
static int frameBoundary = FALSE;

// The execution speed we're running at.
static enum {SPEED_NORMAL, SPEED_NOTHROTTLE, SPEED_TURBO, SPEED_MAXIMUM} speedmode = SPEED_NORMAL;

// Rerecord count skip mode
static int skipRerecords = FALSE;

// Used by the registry to find our functions
static const char *frameAdvanceThread = "PCSX.FrameAdvance";
static const char *memoryWatchTable = "PCSX.Memory";
static const char *memoryValueTable = "PCSX.MemValues";
static const char *guiCallbackTable = "PCSX.GUI";

// True if there's a thread waiting to run after a run of frame-advance.
static int frameAdvanceWaiting = FALSE;

// Transparency strength. 255=opaque, 0=so transparent it's invisible
static int transparencyModifier = 255;

// Our joypads.
static uint32 lua_joypads[2];
static uint8 lua_joypads_used;

static uint8 gui_enabled = TRUE;
static enum { GUI_USED_SINCE_LAST_DISPLAY, GUI_USED_SINCE_LAST_FRAME, GUI_CLEAR } gui_used = GUI_CLEAR;
static uint8 *gui_data = NULL;

// Protects Lua calls from going nuts.
// We set this to a big number like 1000 and decrement it
// over time. The script gets knifed once this reaches zero.
static int numTries;

// Look in pcsx.h for macros named like JOY_UP to determine the order.
static const char *button_mappings[] = {
	"select", "l3", "r3", "start", "up", "right", "down", "left",
	"l2", "r2", "l1", "r1", "triangle", "circle", "x", "square"
};

#ifdef _MSC_VER
	#define snprintf _snprintf
	#define vscprintf _vscprintf
#else
	#define stricmp strcasecmp
	#define strnicmp strncasecmp
	#define __forceinline __attribute__((always_inline))
#endif

static const char* luaCallIDStrings [] =
{
	"CALL_BEFOREEMULATION",
	"CALL_AFTEREMULATION",
	"CALL_BEFOREEXIT",
};

static char* rawToCString(lua_State* L, int idx=0);
static const char* toCString(lua_State* L, int idx=0);

// LuaWriteInform is very slow, so we'll only use it if memory.register was used in this session.
static int usingMemoryRegister=0;


/**
 * Resets emulator speed / pause states after script exit.
 * (Actually, PCSX doesn't do any of these. They were very annoying.)
 */
static void PCSX_LuaOnStop() {
	luaRunning = FALSE;
	lua_joypads_used = 0;
	gui_used = GUI_CLEAR;
}


/**
 * Asks Lua if it wants control of the emulator's speed.
 * Returns 0 if no, 1 if yes. If yes, caller should also
 * consult PCSX_LuaFrameSkip().
 */
int PCSX_LuaSpeed() {
	if (!LUA || !luaRunning)
		return 0;

	switch (speedmode) {
	case SPEED_NOTHROTTLE:
	case SPEED_TURBO:
	case SPEED_MAXIMUM:
		return 1;
	case SPEED_NORMAL:
	default:
		return 0;
	}
}


/**
 * Asks Lua if it wants control whether this frame is skipped.
 * Returns 0 if no, 1 if frame should be skipped, -1 if it should not be.
 */
int PCSX_LuaFrameSkip() {
	if (!LUA || !luaRunning)
		return 0;

	switch (speedmode) {
	case SPEED_NORMAL:
		return 0;
	case SPEED_NOTHROTTLE:
		return -1;
	case SPEED_TURBO:
		return 0;
	case SPEED_MAXIMUM:
		return 1;
	}

	return 0;
}


/**
 * When code determines that a write has occurred
 * (not necessarily worth informing Lua), call this.
 */
void PCSX_LuaWriteInform() {
	if (!LUA || !luaRunning || !usingMemoryRegister) return;
	// Nuke the stack, just in case.
	lua_settop(LUA,0);

	lua_getfield(LUA, LUA_REGISTRYINDEX, memoryWatchTable);
	lua_pushnil(LUA);
	while (lua_next(LUA, 1) != 0)
	{
		unsigned int addr = luaL_checkinteger(LUA, 2);
		lua_Integer value;
		lua_getfield(LUA, LUA_REGISTRYINDEX, memoryValueTable);
		lua_pushvalue(LUA, 2);
		lua_gettable(LUA, 4);
		value = luaL_checkinteger(LUA, 5);
		if (psxMu8(addr) != value)
		{
			int res;

			// Value changed; update & invoke the Lua callback
			lua_pushinteger(LUA, addr);
			lua_pushinteger(LUA, psxMs8(addr));
			lua_settable(LUA, 4);
			lua_pop(LUA, 2);

			numTries = 1000;
			res = lua_pcall(LUA, 0, 0, 0);
			if (res) {
				const char *err = lua_tostring(LUA, -1);
				
#ifdef WIN32
				MessageBox(gApp.hWnd, err, "Lua Engine", MB_OK);
#else
				fprintf(stderr, "Lua error: %s\n", err);
#endif
			}
		}
		lua_settop(LUA, 2);
	}
	lua_settop(LUA, 0);
}

///////////////////////////



// pcsx.speedmode(string mode)
//
//   Takes control of the emulation speed
//   of the system. Normal is normal speed (60fps, 50 for PAL),
//   nothrottle disables speed control but renders every frame,
//   turbo renders only a few frames in order to speed up emulation,
//   maximum renders no frames
static int pcsx_speedmode(lua_State *L) {
	const char *mode = luaL_checkstring(L,1);
	
	if (strcasecmp(mode, "normal")==0) {
		speedmode = SPEED_NORMAL;
		SetEmulationSpeed(EMUSPEED_NORMAL);
	} else if (strcasecmp(mode, "nothrottle")==0) {
		speedmode = SPEED_NOTHROTTLE;
		SetEmulationSpeed(EMUSPEED_FASTEST);
	} else if (strcasecmp(mode, "turbo")==0) {
		speedmode = SPEED_TURBO;
		SetEmulationSpeed(EMUSPEED_TURBO);
	} else if (strcasecmp(mode, "maximum")==0) {
		speedmode = SPEED_MAXIMUM;
		SetEmulationSpeed(EMUSPEED_MAXIMUM);
	} else
		luaL_error(L, "Invalid mode %s to pcsx.speedmode",mode);

	return 0;
}


// pcsx.frameadvance()
//
//  Executes a frame advance. Occurs by yielding the coroutine, then re-running
//  when we break out.
static int pcsx_frameadvance(lua_State *L) {
	// We're going to sleep for a frame-advance. Take notes.

	if (frameAdvanceWaiting) 
		return luaL_error(L, "can't call pcsx.frameadvance() from here");

	frameAdvanceWaiting = TRUE;

	// Now we can yield to the main 
	return lua_yield(L, 0);
}


// pcsx.pause()
//
//  Pauses the emulator, function "waits" until the user unpauses.
//  This function MAY be called from a non-frame boundary, but the frame
//  finishes executing anyways. In this case, the function returns immediately.
static int pcsx_pause(lua_State *L) {
	iPause=1;
	speedmode = SPEED_NORMAL;

	// If it's on a frame boundary, we also yield.
	frameAdvanceWaiting = TRUE;
	return lua_yield(L, 0);
}


// pcsx.unpause()
static int pcsx_unpause(lua_State *L) {
	iPause=0;

	return lua_yield(L, 0);
}

static inline bool isalphaorunderscore(char c)
{
	return isalpha(c) || c == '_';
}

static std::vector<const void*> s_tableAddressStack; // prevents infinite recursion of a table within a table (when cycle is found, print something like table:parent)
static std::vector<const void*> s_metacallStack; // prevents infinite recursion if something's __tostring returns another table that contains that something (when cycle is found, print the inner result without using __tostring)

#define APPENDPRINT { int _n = snprintf(ptr, remaining,
#define END ); if(_n >= 0) { ptr += _n; remaining -= _n; } else { remaining = 0; } }
static void toCStringConverter(lua_State* L, int i, char*& ptr, int& remaining)
{
	if(remaining <= 0)
		return;

//	const char* str = ptr; // for debugging

	// if there is a __tostring metamethod then call it
	int usedMeta = luaL_callmeta(L, i, "__tostring");
	if(usedMeta)
	{
		std::vector<const void*>::const_iterator foundCycleIter = std::find(s_metacallStack.begin(), s_metacallStack.end(), lua_topointer(L,i));
		if(foundCycleIter != s_metacallStack.end())
		{
			lua_pop(L, 1);
			usedMeta = false;
		}
		else
		{
			s_metacallStack.push_back(lua_topointer(L,i));
			i = lua_gettop(L);
		}
	}

	switch(lua_type(L, i))
	{
		case LUA_TNONE: break;
		case LUA_TNIL: APPENDPRINT "nil" END break;
		case LUA_TBOOLEAN: APPENDPRINT lua_toboolean(L,i) ? "true" : "false" END break;
		case LUA_TSTRING: APPENDPRINT "%s",lua_tostring(L,i) END break;
		case LUA_TNUMBER: APPENDPRINT "%.12g",lua_tonumber(L,i) END break;
		case LUA_TFUNCTION: 
			if((L->base + i-1)->value.gc->cl.c.isC)
			{
				//lua_CFunction func = lua_tocfunction(L, i);
				//std::map<lua_CFunction, const char*>::iterator iter = s_cFuncInfoMap.find(func);
				//if(iter == s_cFuncInfoMap.end())
					goto defcase;
				//APPENDPRINT "function(%s)", iter->second END 
			}
			else
			{
				APPENDPRINT "function(" END 
				Proto* p = (L->base + i-1)->value.gc->cl.l.p;
				int numParams = p->numparams + (p->is_vararg?1:0);
				for (int n=0; n<p->numparams; n++)
				{
					APPENDPRINT "%s", getstr(p->locvars[n].varname) END 
					if(n != numParams-1)
						APPENDPRINT "," END
				}
				if(p->is_vararg)
					APPENDPRINT "..." END
				APPENDPRINT ")" END
			}
			break;
defcase:default: APPENDPRINT "%s:%p",luaL_typename(L,i),lua_topointer(L,i) END break;
		case LUA_TTABLE:
		{
			// first make sure there's enough stack space
			if(!lua_checkstack(L, 4))
			{
				// note that even if lua_checkstack never returns false,
				// that doesn't mean we didn't need to call it,
				// because calling it retrieves stack space past LUA_MINSTACK
				goto defcase;
			}

			std::vector<const void*>::const_iterator foundCycleIter = std::find(s_tableAddressStack.begin(), s_tableAddressStack.end(), lua_topointer(L,i));
			if(foundCycleIter != s_tableAddressStack.end())
			{
				int parentNum = s_tableAddressStack.end() - foundCycleIter;
				if(parentNum > 1)
					APPENDPRINT "%s:parent^%d",luaL_typename(L,i),parentNum END
				else
					APPENDPRINT "%s:parent",luaL_typename(L,i) END
			}
			else
			{
				s_tableAddressStack.push_back(lua_topointer(L,i));
//				struct Scope { ~Scope(){ s_tableAddressStack.pop_back(); } } scope;

				APPENDPRINT "{" END

				lua_pushnil(L); // first key
				int keyIndex = lua_gettop(L);
				int valueIndex = keyIndex + 1;
				bool first = true;
				bool skipKey = true; // true if we're still in the "array part" of the table
				lua_Number arrayIndex = (lua_Number)0;
				while(lua_next(L, i))
				{
					if(first)
						first = false;
					else
						APPENDPRINT ", " END
					if(skipKey)
					{
						arrayIndex += (lua_Number)1;
						bool keyIsNumber = (lua_type(L, keyIndex) == LUA_TNUMBER);
						skipKey = keyIsNumber && (lua_tonumber(L, keyIndex) == arrayIndex);
					}
					if(!skipKey)
					{
						bool keyIsString = (lua_type(L, keyIndex) == LUA_TSTRING);
						bool invalidLuaIdentifier = (!keyIsString || !isalphaorunderscore(*lua_tostring(L, keyIndex)));
						if(invalidLuaIdentifier) {
							if(keyIsString)
								APPENDPRINT "['" END
							else
								APPENDPRINT "[" END
						}

						toCStringConverter(L, keyIndex, ptr, remaining); // key

						if(invalidLuaIdentifier)
							if(keyIsString)
								APPENDPRINT "']=" END
							else
								APPENDPRINT "]=" END
						else
							APPENDPRINT "=" END
					}

					bool valueIsString = (lua_type(L, valueIndex) == LUA_TSTRING);
					if(valueIsString)
						APPENDPRINT "'" END

					toCStringConverter(L, valueIndex, ptr, remaining); // value

					if(valueIsString)
						APPENDPRINT "'" END

					lua_pop(L, 1);

					if(remaining <= 0)
					{
						lua_settop(L, keyIndex-1); // stack might not be clean yet if we're breaking early
						break;
					}
				}
				APPENDPRINT "}" END
			}
		}	break;
	}

	if(usedMeta)
	{
		s_metacallStack.pop_back();
		lua_pop(L, 1);
	}
}

static const int s_tempStrMaxLen = 64 * 1024;
static char s_tempStr [s_tempStrMaxLen];

static char* rawToCString(lua_State* L, int idx)
{
	int a = idx>0 ? idx : 1;
	int n = idx>0 ? idx : lua_gettop(L);

	char* ptr = s_tempStr;
	*ptr = 0;

	int remaining = s_tempStrMaxLen;
	for(int i = a; i <= n; i++)
	{
		toCStringConverter(L, i, ptr, remaining);
		if(i != n)
			APPENDPRINT " " END
	}

	if(remaining < 3)
	{
		while(remaining < 6)
			remaining++, ptr--;
		APPENDPRINT "..." END
	}
	APPENDPRINT "\r\n" END
	// the trailing newline is so print() can avoid having to do wasteful things to print its newline
	// (string copying would be wasteful and calling info.print() twice can be extremely slow)
	// at the cost of functions that don't want the newline needing to trim off the last two characters
	// (which is a very fast operation and thus acceptable in this case)

	return s_tempStr;
}
#undef APPENDPRINT
#undef END


// replacement for luaB_tostring() that is able to show the contents of tables (and formats numbers better, and show function prototypes)
// can be called directly from lua via tostring(), assuming tostring hasn't been reassigned
static int tostring(lua_State *L)
{
	char* str = rawToCString(L);
	str[strlen(str)-2] = 0; // hack: trim off the \r\n (which is there to simplify the print function's task)
	lua_pushstring(L, str);
	return 1;
}

// like rawToCString, but will check if the global Lua function tostring()
// has been replaced with a custom function, and call that instead if so
static const char* toCString(lua_State* L, int idx)
{
	int a = idx>0 ? idx : 1;
	int n = idx>0 ? idx : lua_gettop(L);
	lua_getglobal(L, "tostring");
	lua_CFunction cf = lua_tocfunction(L,-1);
	if(cf == tostring) // optimization: if using our own C tostring function, we can bypass the call through Lua and all the string object allocation that would entail
	{
		lua_pop(L,1);
		return rawToCString(L, idx);
	}
	else // if the user overrided the tostring function, we have to actually call it and store the temporarily allocated string it returns
	{
		lua_pushstring(L, "");
		for (int i=a; i<=n; i++) {
			lua_pushvalue(L, -2);  // function to be called
			lua_pushvalue(L, i);   // value to print
			lua_call(L, 1, 1);
			if(lua_tostring(L, -1) == NULL)
				luaL_error(L, LUA_QL("tostring") " must return a string to " LUA_QL("print"));
			lua_pushstring(L, (i<n) ? " " : "\r\n");
			lua_concat(L, 3);
		}
		const char* str = lua_tostring(L, -1);
		strncpy(s_tempStr, str, s_tempStrMaxLen);
		s_tempStr[s_tempStrMaxLen-1] = 0;
		lua_pop(L, 2);
		return s_tempStr;
	}
}

// replacement for luaB_print() that goes to the appropriate textbox instead of stdout
static int print(lua_State *L)
{
	const char* str = toCString(L);

	int uid = info_uid;//luaStateToUIDMap[L->l_G->mainthread];
	//LuaContextInfo& info = GetCurrentInfo();

	if(info_print)
		info_print(uid, str);
	else
		puts(str);

	//worry(L, 100);
	return 0;
}

char pcsx_message_buffer[1024];
// pcsx.message(string msg)
//
//  Displays the given message on the screen.
static int pcsx_message(lua_State *L) {
	const char *msg = luaL_checkstring(L,1);
	sprintf(pcsx_message_buffer, "%s", msg);
	GPU_displayText(pcsx_message_buffer);

	return 0;
}

// provides an easy way to copy a table from Lua
// (simple assignment only makes an alias, but sometimes an independent table is desired)
// currently this function only performs a shallow copy,
// but I think it should be changed to do a deep copy (possibly of configurable depth?)
// that maintains the internal table reference structure
static int copytable(lua_State *L)
{
	int origIndex = 1; // we only care about the first argument
	int origType = lua_type(L, origIndex);
	if(origType == LUA_TNIL)
	{
		lua_pushnil(L);
		return 1;
	}
	if(origType != LUA_TTABLE)
	{
		luaL_typerror(L, 1, lua_typename(L, LUA_TTABLE));
		lua_pushnil(L);
		return 1;
	}
	
	lua_createtable(L, lua_objlen(L,1), 0);
	int copyIndex = lua_gettop(L);

	lua_pushnil(L); // first key
	int keyIndex = lua_gettop(L);
	int valueIndex = keyIndex + 1;

	while(lua_next(L, origIndex))
	{
		lua_pushvalue(L, keyIndex);
		lua_pushvalue(L, valueIndex);
		lua_rawset(L, copyIndex); // copytable[key] = value
		lua_pop(L, 1);
	}

	// copy the reference to the metatable as well, if any
	if(lua_getmetatable(L, origIndex))
		lua_setmetatable(L, copyIndex);

	return 1; // return the new table
}

// because print traditionally shows the address of tables,
// and the print function I provide instead shows the contents of tables,
// I also provide this function
// (otherwise there would be no way to see a table's address, AFAICT)
static int addressof(lua_State *L)
{
	const void* ptr = lua_topointer(L,-1);
	lua_pushinteger(L, (lua_Integer)ptr);
	return 1;
}

static int pcsx_registerbefore(lua_State *L) {
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEMULATION]);
	lua_insert(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEMULATION]);
	return 1;
}


static int pcsx_registerafter(lua_State *L) {
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATION]);
	lua_insert(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATION]);
	return 1;
}


static int pcsx_registerexit(lua_State *L) {
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);
	lua_insert(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);
	return 1;
}


// int pcsx.lagcount()
int pcsx_lagcount(lua_State *L) {
	lua_pushinteger(L, Movie.lagCounter);
	return 1;
}


// boolean pcsx.lagged()
int pcsx_lagged(lua_State *L) {
	int lagged = 0;
	if(iJoysToPoll == 2)
		lagged = 1;
	lua_pushboolean(L, lagged);
	return 1;
}


static int memory_readbyte(lua_State *L)
{
	lua_pushinteger(L, psxMu8(luaL_checkinteger(L,1)));
	return 1;
}

static int memory_readbytesigned(lua_State *L) {
	signed char c = psxMs8(luaL_checkinteger(L,1));
	lua_pushinteger(L, c);
	return 1;
}

static int memory_readword(lua_State *L)
{
	lua_pushinteger(L, psxMu16(luaL_checkinteger(L,1)));
	return 1;
}

static int memory_readwordsigned(lua_State *L) {
	signed short c = psxMs16(luaL_checkinteger(L,1));
	lua_pushinteger(L, c);
	return 1;
}

static int memory_readdword(lua_State *L)
{
	uint32 addr = luaL_checkinteger(L,1);
	uint32 val = psxMu32(addr);

	// lua_pushinteger doesn't work properly for 32bit system, does it?
	if (val >= 0x80000000 && sizeof(int) <= 4)
		lua_pushnumber(L, val);
	else
		lua_pushinteger(L, val);
	return 1;
}

static int memory_readdwordsigned(lua_State *L) {
	uint32 addr = luaL_checkinteger(L,1);
	int32 val = psxMs32(addr);

	lua_pushinteger(L, val);
	return 1;
}

static int memory_readbyterange(lua_State *L) {
	int a,n;
	uint32 address = luaL_checkinteger(L,1);
	int length = luaL_checkinteger(L,2);

	if(length < 0)
	{
		address += length;
		length = -length;
	}

	// push the array
	lua_createtable(L, abs(length), 0);

	// put all the values into the (1-based) array
	for(a = address, n = 1; n <= length; a++, n++)
	{
		unsigned char value = psxMu8(a);
		lua_pushinteger(L, value);
		lua_rawseti(L, -2, n);
	}

	return 1;
}


static int memory_writebyte(lua_State *L)
{
	psxMemWrite8(luaL_checkinteger(L,1), luaL_checkinteger(L,2));
	return 0;
}

static int memory_writeword(lua_State *L)
{
	psxMemWrite16(luaL_checkinteger(L,1), luaL_checkinteger(L,2));
	return 0;
}

static int memory_writedword(lua_State *L)
{
	psxMemWrite32(luaL_checkinteger(L,1), luaL_checkinteger(L,2));
	return 0;
}


// memory.registerwrite(int address, function func)
//
//  Calls the given function when the indicated memory address is
//  written to. No args are given to the function. The write has already
//  occurred, so the new address is readable.
static int memory_registerwrite(lua_State *L) {
	// Check args
	unsigned int addr = luaL_checkinteger(L, 1);
	if (lua_type(L,2) != LUA_TNIL && lua_type(L,2) != LUA_TFUNCTION)
		luaL_error(L, "function or nil expected in arg 2 to memory.register");
	
	
	// Check the address range
	if (addr > 0x200000)
		luaL_error(L, "arg 1 should be between 0x0000 and 0x200000");

	// Commit it to the registery
	lua_getfield(L, LUA_REGISTRYINDEX, memoryWatchTable);
	lua_pushvalue(L,1);
	lua_pushvalue(L,2);
	lua_settable(L, -3);
	lua_getfield(L, LUA_REGISTRYINDEX, memoryValueTable);
	lua_pushvalue(L,1);
	if (lua_isnil(L,2)) lua_pushnil(L);
	else lua_pushinteger(L, psxMu8(addr));
	lua_settable(L, -3);
	
	if(!usingMemoryRegister)
		usingMemoryRegister=1;
	return 0;
}


// table joypad.read(int which = 1)
//
//  Reads the joypads as inputted by the user.
static int joy_get_internal(lua_State *L, bool reportUp, bool reportDown) {
	unsigned short buttons=0;
	int which;
	int i;
	
	// Reads the joypads as inputted by the user
	which = luaL_checkinteger(L,1);
	if(which == 1) {
		PadDataS padd;
		PAD1_readPort1(&padd);
		buttons = padd.buttonStatus^0xffff;
	}
	else if(which == 2) {
		PadDataS padd;
		PAD2_readPort2(&padd);
		buttons = padd.buttonStatus^0xffff;
	}
	else
		luaL_error(L,"Invalid input port (valid range 1-2, specified %d)", which);

	lua_newtable(L);
	
	for (i = 0; i < 16; i++) {
		bool pressed = (buttons & (1<<i))!=0;
		if ((pressed && reportDown) || (!pressed && reportUp)) {
			lua_pushboolean(L, pressed);
			lua_setfield(L, -2, button_mappings[i]);
		}
	}
	
	return 1;
}
// joypad.get(which)
// returns a table of every game button,
// true meaning currently-held and false meaning not-currently-held
// (as of last frame boundary)
// this WILL read input from a currently-playing movie
static int joypad_get(lua_State *L)
{
	return joy_get_internal(L, true, true);
}
// joypad.getdown(which)
// returns a table of every game button that is currently held
static int joypad_getdown(lua_State *L)
{
	return joy_get_internal(L, false, true);
}
// joypad.getup(which)
// returns a table of every game button that is not currently held
static int joypad_getup(lua_State *L)
{
	return joy_get_internal(L, true, false);
}


// joypad.set(int which, table buttons)
//
//   Sets the given buttons to be pressed during the next
//   frame advance. The table should have the right 
//   keys (no pun intended) set.
static int joypad_set(lua_State *L) {
	int which;
	int i;

	// Which joypad we're tampering with
	which = luaL_checkinteger(L,1);
	if (which < 1 || which > 2) {
		luaL_error(L,"Invalid output port (valid range 1-2, specified %d)", which);
	}

	// And the table of buttons.
	luaL_checktype(L,2,LUA_TTABLE);

	// Set up for taking control of the indicated controller
	lua_joypads_used |= 1 << (which-1);
	lua_joypads[which-1] = 0;

	for (i=0; i < 16; i++) {
		lua_getfield(L, 2, button_mappings[i]);
		if (!lua_isnil(L,-1))
			lua_joypads[which-1] |= 1 << i;
		lua_pop(L,1);
	}
	
	return 0;
}


// Helper function to convert a savestate object to the filename it represents.
static char *savestateobj2filename(lua_State *L, int offset) {
	
	// First we get the metatable of the indicated object
	int result = lua_getmetatable(L, offset);

	if (!result)
		luaL_error(L, "object not a savestate object");
	
	// Also check that the type entry is set
	lua_getfield(L, -1, "__metatable");
	if (strcmp(lua_tostring(L,-1), "PCSX Savestate") != 0)
		luaL_error(L, "object not a savestate object");
	lua_pop(L,1);
	
	// Now, get the field we want
	lua_getfield(L, -1, "filename");
	
	// Return it
	return (char *) lua_tostring(L, -1);
}


// Helper function for garbage collection.
static int savestate_gc(lua_State *L) {
	const char *filename;

	// The object we're collecting is on top of the stack
	lua_getmetatable(L,1);
	
	// Get the filename
	lua_getfield(L, -1, "filename");
	filename = lua_tostring(L,-1);

	// Delete the file
	remove(filename);
	
	// We exit, and the garbage collector takes care of the rest.
	return 0;
}


// object savestate.create(int which = nil)
//
//  Creates an object used for savestates.
//  The object can be associated with a player-accessible savestate
//  ("which" between 1 and 10) or not (which == nil).
static int savestate_create(lua_State *L) {
	int which = -1;
	char *filename;

	if (lua_gettop(L) >= 1) {
		which = luaL_checkinteger(L, 1);
		if (which < 1 || which > 10) {
			luaL_error(L, "invalid player's savestate %d", which);
		}
	}
	

	if (which > 0) {
		// Find an appropriate filename. This is OS specific, unfortunately.
		// So I turned the filename selection code into my bitch. :)
		// Numbers are 0 through 9 though.
		filename = GetSavestateFilename(which -1);
	}
	else {
		filename = tempnam(NULL, "snlua");
	}
	
	// Our "object". We don't care about the type, we just need the memory and GC services.
	lua_newuserdata(L,1);
	
	// The metatable we use, protected from Lua and contains garbage collection info and stuff.
	lua_newtable(L);
	
	// First, we must protect it
	lua_pushstring(L, "PCSX Savestate");
	lua_setfield(L, -2, "__metatable");
	
	
	// Now we need to save the file itself.
	lua_pushstring(L, filename);
	lua_setfield(L, -2, "filename");
	
	// If it's an anonymous savestate, we must delete the file from disk should it be gargage collected
	if (which < 0) {
		lua_pushcfunction(L, savestate_gc);
		lua_setfield(L, -2, "__gc");
	}
	
	// Set the metatable
	lua_setmetatable(L, -2);

	// The filename was allocated using malloc. Do something about that.
	free(filename);
	
	// Awesome. Return the object
	return 1;
	
}


// savestate.save(object state)
//
//   Saves a state to the given object.
static int savestate_save(lua_State *L) {
	char *filename = savestateobj2filename(L,1);

	// Save states are very expensive. They take time.
	numTries--;

	SaveState(filename);
	return 0;
}

// savestate.load(object state)
//
//   Loads the given state
static int savestate_load(lua_State *L) {
	char *filename = savestateobj2filename(L,1);

	numTries--;

	LoadState(filename);
	return 0;

}


// int movie.framecount()
//
//   Gets the frame counter for the movie
int movie_framecount(lua_State *L) {
	lua_pushinteger(L, Movie.currentFrame);
	return 1;
}


// string movie.mode()
//
//   "record", "playback" or nil
static int movie_mode(lua_State *L) {
	if (Movie.mode == MOVIEMODE_RECORD)
		lua_pushstring(L, "record");
	else if (Movie.mode == MOVIEMODE_PLAY)
		lua_pushstring(L, "playback");
	else
		lua_pushnil(L);
	return 1;
}

static int movie_isactive(lua_State *L) {
	lua_pushboolean(L, Movie.mode != MOVIEMODE_INACTIVE);
	return 1;
}

static int movie_isrecording(lua_State *L) {
	lua_pushboolean(L, Movie.mode == MOVIEMODE_RECORD);
	return 1;
}

static int movie_isplaying(lua_State *L) {
	lua_pushboolean(L, Movie.mode == MOVIEMODE_PLAY);
	return 1;
}


static int movie_getname(lua_State *L) {
	if(Movie.mode != MOVIEMODE_INACTIVE)
		lua_pushstring(L, Movie.movieFilename);
	else
		lua_pushstring(L, "");
	return 1;
}

static int movie_getlength(lua_State *L) {
	if(Movie.mode != MOVIEMODE_INACTIVE)
		lua_pushinteger(L, Movie.totalFrames);
	else
		lua_pushinteger(L, 0);
	return 1;
}

static int movie_rerecordcount(lua_State *L) {
	if(Movie.mode != MOVIEMODE_INACTIVE)
		lua_pushinteger(L, Movie.rerecordCount);
	else
		lua_pushinteger(L, 0);
	return 1;
}

static int movie_setrerecordcount(lua_State *L) {
	if(Movie.mode != MOVIEMODE_INACTIVE)
		Movie.rerecordCount = luaL_checkinteger(L, 1);
	return 0;
}


static int movie_rerecordcounting(lua_State *L) {
	if (lua_gettop(L) == 0)
		luaL_error(L, "no parameters specified");

	skipRerecords = lua_toboolean(L,1);
	return 0;
}


// movie.stop()
//
//   Stops movie playback/recording. Bombs out if movie is not running.
static int movie_stop(lua_State *L) {
	if (Movie.mode == MOVIEMODE_INACTIVE)
		luaL_error(L, "no movie");
	
	MOV_StopMovie();
	return 0;

}

int LUA_SCREEN_WIDTH  = 640;
int LUA_SCREEN_HEIGHT = 512;

// Common code by the gui library: make sure the screen array is ready
static void gui_prepare() {
	int x,y;
	if (!gui_data)
		gui_data = (uint8 *) malloc(LUA_SCREEN_WIDTH * LUA_SCREEN_HEIGHT * 4);
	if (gui_used != GUI_USED_SINCE_LAST_DISPLAY) {
		for (y = 0; y < LUA_SCREEN_HEIGHT; y++) {
			for (x=0; x < LUA_SCREEN_WIDTH; x++) {
				if (gui_data[(y*LUA_SCREEN_WIDTH+x)*4+3] != 0)
					gui_data[(y*LUA_SCREEN_WIDTH+x)*4+3] = 0;
			}
		}
	}
//		if (gui_used != GUI_USED_SINCE_LAST_DISPLAY) /* mz: 10% slower on my system */
//			memset(gui_data,0,LUA_SCREEN_WIDTH * LUA_SCREEN_HEIGHT * 4);
	gui_used = GUI_USED_SINCE_LAST_DISPLAY;
}


// pixform for lua graphics
#define BUILD_PIXEL_ARGB8888(A,R,G,B) (((int) (A) << 24) | ((int) (R) << 16) | ((int) (G) << 8) | (int) (B))
#define DECOMPOSE_PIXEL_ARGB8888(PIX,A,R,G,B) { (A) = ((PIX) >> 24) & 0xff; (R) = ((PIX) >> 16) & 0xff; (G) = ((PIX) >> 8) & 0xff; (B) = (PIX) & 0xff; }
#define LUA_BUILD_PIXEL BUILD_PIXEL_ARGB8888
#define LUA_DECOMPOSE_PIXEL DECOMPOSE_PIXEL_ARGB8888
#define LUA_PIXEL_A(PIX) (((PIX) >> 24) & 0xff)
#define LUA_PIXEL_R(PIX) (((PIX) >> 16) & 0xff)
#define LUA_PIXEL_G(PIX) (((PIX) >> 8) & 0xff)
#define LUA_PIXEL_B(PIX) ((PIX) & 0xff)

// I'm going to use this a lot in here
#define swap(T, one, two) { \
	T temp = one; \
	one = two;    \
	two = temp;   \
}

// write a pixel to buffer
static inline void blend32(uint32 *dstPixel, uint32 colour)
{
	uint8 *dst = (uint8*) dstPixel;
	int a, r, g, b;
	LUA_DECOMPOSE_PIXEL(colour, a, r, g, b);

	if (a == 255 || dst[3] == 0) {
		// direct copy
		*(uint32*)(dst) = colour;
	}
	else if (a == 0) {
		// do not copy
	}
	else {
		// alpha-blending
		int a_dst = ((255 - a) * dst[3] + 128) / 255;
		int a_new = a + a_dst;

		dst[0] = (uint8) ((( dst[0] * a_dst + b * a) + (a_new / 2)) / a_new);
		dst[1] = (uint8) ((( dst[1] * a_dst + g * a) + (a_new / 2)) / a_new);
		dst[2] = (uint8) ((( dst[2] * a_dst + r * a) + (a_new / 2)) / a_new);
		dst[3] = (uint8) a_new;
	}
}

// check if a pixel is in the lua canvas
static inline uint8 gui_check_boundary(int x, int y) {
	return !(x < 0 || x >= LUA_SCREEN_WIDTH || y < 0 || y >= LUA_SCREEN_HEIGHT);
}

// write a pixel to gui_data (do not check boundaries for speedup)
static inline void gui_drawpixel_fast(int x, int y, uint32 colour) {
	//gui_prepare();
	blend32((uint32*) &gui_data[(y*LUA_SCREEN_WIDTH+x)*4], colour);
}

// write a pixel to gui_data (check boundaries)
static inline void gui_drawpixel_internal(int x, int y, uint32 colour) {
	//gui_prepare();
	if (gui_check_boundary(x, y))
		gui_drawpixel_fast(x, y, colour);
}

// draw a line on gui_data (checks boundaries)
static void gui_drawline_internal(int x1, int y1, int x2, int y2, uint8 lastPixel, uint32 colour) {

	//gui_prepare();

	// Note: New version of Bresenham's Line Algorithm
	// http://groups.google.co.jp/group/rec.games.roguelike.development/browse_thread/thread/345f4c42c3b25858/29e07a3af3a450e6?show_docid=29e07a3af3a450e6

	int swappedx = 0;
	int swappedy = 0;

	int xtemp = x1-x2;
	int ytemp = y1-y2;

	int delta_x;
	int delta_y;

	signed char ix;
	signed char iy;

	if (xtemp == 0 && ytemp == 0) {
		gui_drawpixel_internal(x1, y1, colour);
		return;
	}
	if (xtemp < 0) {
		xtemp = -xtemp;
		swappedx = 1;
	}
	if (ytemp < 0) {
		ytemp = -ytemp;
		swappedy = 1;
	}

	delta_x = xtemp << 1;
	delta_y = ytemp << 1;

	ix = x1 > x2?1:-1;
	iy = y1 > y2?1:-1;

	if (lastPixel)
		gui_drawpixel_internal(x2, y2, colour);

	if (delta_x >= delta_y) {
		int error = delta_y - (delta_x >> 1);

		while (x2 != x1) {
			if (error == 0 && !swappedx)
				gui_drawpixel_internal(x2+ix, y2, colour);
			if (error >= 0) {
				if (error || (ix > 0)) {
					y2 += iy;
					error -= delta_x;
				}
			}
			x2 += ix;
			gui_drawpixel_internal(x2, y2, colour);
			if (error == 0 && swappedx)
				gui_drawpixel_internal(x2, y2+iy, colour);
			error += delta_y;
		}
	}
	else {
		int error = delta_x - (delta_y >> 1);

		while (y2 != y1) {
			if (error == 0 && !swappedy)
				gui_drawpixel_internal(x2, y2+iy, colour);
			if (error >= 0) {
				if (error || (iy > 0)) {
					x2 += ix;
					error -= delta_y;
				}
			}
			y2 += iy;
			gui_drawpixel_internal(x2, y2, colour);
			if (error == 0 && swappedy)
				gui_drawpixel_internal(x2+ix, y2, colour);
			error += delta_x;
		}
	}
}

// draw a rect on gui_data
static void gui_drawbox_internal(int x1, int y1, int x2, int y2, uint32 colour) {

	if (x1 > x2) 
		swap(int, x1, x2);
	if (y1 > y2) 
		swap(int, y1, y2);
	if (x1 < 0)
		x1 = -1;
	if (y1 < 0)
		y1 = -1;
	if (x2 >= LUA_SCREEN_WIDTH)
		x2 = LUA_SCREEN_WIDTH;
	if (y2 >= LUA_SCREEN_HEIGHT)
		y2 = LUA_SCREEN_HEIGHT;

	//gui_prepare();

	gui_drawline_internal(x1, y1, x2, y1, TRUE, colour);
	gui_drawline_internal(x1, y2, x2, y2, TRUE, colour);
	gui_drawline_internal(x1, y1, x1, y2, TRUE, colour);
	gui_drawline_internal(x2, y1, x2, y2, TRUE, colour);
}

// draw a circle on gui_data
//static void gui_drawcircle_internal(int x0, int y0, int radius, uint32 colour) {
//
//	int f;
//	int ddF_x;
//	int ddF_y;
//	int x;
//	int y;
//
//	//gui_prepare();
//
//	if (radius < 0)
//		radius = -radius;
//	if (radius == 0)
//		return;
//	if (radius == 1) {
//		gui_drawpixel_internal(x0, y0, colour);
//		return;
//	}
//
//	// http://en.wikipedia.org/wiki/Midpoint_circle_algorithm
//
//	f = 1 - radius;
//	ddF_x = 1;
//	ddF_y = -2 * radius;
//	x = 0;
//	y = radius;
//
//	gui_drawpixel_internal(x0, y0 + radius, colour);
//	gui_drawpixel_internal(x0, y0 - radius, colour);
//	gui_drawpixel_internal(x0 + radius, y0, colour);
//	gui_drawpixel_internal(x0 - radius, y0, colour);
// 
//	// same pixel shouldn't be drawed twice,
//	// because each pixel has opacity.
//	// so now the routine gets ugly.
//	while(TRUE)
//	{
//		assert(ddF_x == 2 * x + 1);
//		assert(ddF_y == -2 * y);
//		assert(f == x*x + y*y - radius*radius + 2*x - y + 1);
//		if(f >= 0) 
//		{
//			y--;
//			ddF_y += 2;
//			f += ddF_y;
//		}
//		x++;
//		ddF_x += 2;
//		f += ddF_x;
//		if (x < y) {
//			gui_drawpixel_internal(x0 + x, y0 + y, colour);
//			gui_drawpixel_internal(x0 - x, y0 + y, colour);
//			gui_drawpixel_internal(x0 + x, y0 - y, colour);
//			gui_drawpixel_internal(x0 - x, y0 - y, colour);
//			gui_drawpixel_internal(x0 + y, y0 + x, colour);
//			gui_drawpixel_internal(x0 - y, y0 + x, colour);
//			gui_drawpixel_internal(x0 + y, y0 - x, colour);
//			gui_drawpixel_internal(x0 - y, y0 - x, colour);
//		}
//		else if (x == y) {
//			gui_drawpixel_internal(x0 + x, y0 + y, colour);
//			gui_drawpixel_internal(x0 - x, y0 + y, colour);
//			gui_drawpixel_internal(x0 + x, y0 - y, colour);
//			gui_drawpixel_internal(x0 - x, y0 - y, colour);
//			break;
//		}
//		else
//			break;
//	}
//}

// draw fill rect on gui_data
static void gui_fillbox_internal(int x1, int y1, int x2, int y2, uint32 colour) {

	int ix, iy;

	if (x1 > x2) 
		swap(int, x1, x2);
	if (y1 > y2) 
		swap(int, y1, y2);
	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 >= LUA_SCREEN_WIDTH)
		x2 = LUA_SCREEN_WIDTH - 1;
	if (y2 >= LUA_SCREEN_HEIGHT)
		y2 = LUA_SCREEN_HEIGHT - 1;

	//gui_prepare();

	for (iy = y1; iy <= y2; iy++) {
		for (ix = x1; ix <= x2; ix++) {
			gui_drawpixel_fast(ix, iy, colour);
		}
	}
}

// fill a circle on gui_data
//static void gui_fillcircle_internal(int x0, int y0, int radius, uint32 colour) {
//
//	int f;
//	int ddF_x;
//	int ddF_y;
//	int x;
//	int y;
//
//	//gui_prepare();
//
//	if (radius < 0)
//		radius = -radius;
//	if (radius == 0)
//		return;
//	if (radius == 1) {
//		gui_drawpixel_internal(x0, y0, colour);
//		return;
//	}
//
//	// http://en.wikipedia.org/wiki/Midpoint_circle_algorithm
//
//	f = 1 - radius;
//	ddF_x = 1;
//	ddF_y = -2 * radius;
//	x = 0;
//	y = radius;
//
//	gui_drawline_internal(x0, y0 - radius, x0, y0 + radius, TRUE, colour);
// 
//	while(TRUE)
//	{
//		assert(ddF_x == 2 * x + 1);
//		assert(ddF_y == -2 * y);
//		assert(f == x*x + y*y - radius*radius + 2*x - y + 1);
//		if(f >= 0) 
//		{
//			y--;
//			ddF_y += 2;
//			f += ddF_y;
//		}
//		x++;
//		ddF_x += 2;
//		f += ddF_x;
//
//		if (x < y) {
//			gui_drawline_internal(x0 + x, y0 - y, x0 + x, y0 + y, TRUE, colour);
//			gui_drawline_internal(x0 - x, y0 - y, x0 - x, y0 + y, TRUE, colour);
//			if (f >= 0) {
//				gui_drawline_internal(x0 + y, y0 - x, x0 + y, y0 + x, TRUE, colour);
//				gui_drawline_internal(x0 - y, y0 - x, x0 - y, y0 + x, TRUE, colour);
//			}
//		}
//		else if (x == y) {
//			gui_drawline_internal(x0 + x, y0 - y, x0 + x, y0 + y, TRUE, colour);
//			gui_drawline_internal(x0 - x, y0 - y, x0 - x, y0 + y, TRUE, colour);
//			break;
//		}
//		else
//			break;
//	}
//}


static const struct ColorMapping
{
	const char* name;
	int value;
}
s_colorMapping [] =
{
	{"white",     0xFFFFFFFF},
	{"black",     0x000000FF},
	{"clear",     0x00000000},
	{"gray",      0x7F7F7FFF},
	{"grey",      0x7F7F7FFF},
	{"red",       0xFF0000FF},
	{"orange",    0xFF7F00FF},
	{"yellow",    0xFFFF00FF},
	{"chartreuse",0x7FFF00FF},
	{"green",     0x00FF00FF},
	{"teal",      0x00FF7FFF},
	{"cyan" ,     0x00FFFFFF},
	{"blue",      0x0000FFFF},
	{"purple",    0x7F00FFFF},
	{"magenta",   0xFF00FFFF},
};

/**
 * Converts an integer or a string on the stack at the given
 * offset to a RGB32 colour. Several encodings are supported.
 * The user may construct their own RGB value, given a simple colour name,
 * or an HTML-style "#09abcd" colour. 16 bit reduction doesn't occur at this time.
 */
static inline uint8 str2colour(uint32 *colour, lua_State *L, const char *str) {
	if (str[0] == '#') {
		int color;
		int len;
		int missing;
		sscanf(str+1, "%X", &color);
		len = strlen(str+1);
		missing = max(0, 8-len);
		color <<= missing << 2;
		if(missing >= 2) color |= 0xFF;
		*colour = color;
		return TRUE;
	}
	else {
		unsigned int i;
		if(!strnicmp(str, "rand", 4)) {
			*colour = ((rand()*255/RAND_MAX) << 8) | ((rand()*255/RAND_MAX) << 16) | ((rand()*255/RAND_MAX) << 24) | 0xFF;
			return TRUE;
		}
		for(i = 0; i < sizeof(s_colorMapping)/sizeof(*s_colorMapping); i++) {
			if(!stricmp(str,s_colorMapping[i].name)) {
				*colour = s_colorMapping[i].value;
				return TRUE;
			}
		}
	}
	return FALSE;
}
static inline uint32 gui_getcolour_wrapped(lua_State *L, int offset, uint8 hasDefaultValue, uint32 defaultColour) {
	switch (lua_type(L,offset)) {
	case LUA_TSTRING:
		{
			const char *str = lua_tostring(L,offset);
			uint32 colour;

			if (str2colour(&colour, L, str))
				return colour;
			else {
				if (hasDefaultValue)
					return defaultColour;
				else
					return luaL_error(L, "unknown colour %s", str);
			}
		}
	case LUA_TNUMBER:
		{
			uint32 colour = (uint32) lua_tointeger(L,offset);
			return colour;
		}
	case LUA_TTABLE:
		{
			int color = 0xFF;
			lua_pushnil(L); // first key
			int keyIndex = lua_gettop(L);
			int valueIndex = keyIndex + 1;
//			bool first = true;
			while(lua_next(L, offset))
			{
				bool keyIsString = (lua_type(L, keyIndex) == LUA_TSTRING);
				bool keyIsNumber = (lua_type(L, keyIndex) == LUA_TNUMBER);
				int key = keyIsString ? tolower(*lua_tostring(L, keyIndex)) : (keyIsNumber ? lua_tointeger(L, keyIndex) : 0);
				int value = lua_tointeger(L, valueIndex);
				if(value < 0) value = 0;
				if(value > 255) value = 255;
				switch(key)
				{
				case 1: case 'r': color |= value << 24; break;
				case 2: case 'g': color |= value << 16; break;
				case 3: case 'b': color |= value << 8; break;
				case 4: case 'a': color = (color & ~0xFF) | value; break;
				}
				lua_pop(L, 1);
			}
			return color;
		}	break;
	case LUA_TFUNCTION:
		luaL_error(L, "invalid colour"); // NYI
		return 0;
	default:
		if (hasDefaultValue)
			return defaultColour;
		else
			return luaL_error(L, "invalid colour");
	}
}
static uint32 gui_getcolour(lua_State *L, int offset) {
	uint32 colour;
	int a, r, g, b;

	colour = gui_getcolour_wrapped(L, offset, FALSE, 0);
	a = ((colour & 0xff) * transparencyModifier) / 255;
	if (a > 255) a = 255;
	b = (colour >> 8) & 0xff;
	g = (colour >> 16) & 0xff;
	r = (colour >> 24) & 0xff;
	return LUA_BUILD_PIXEL(a, r, g, b);
}
static uint32 gui_optcolour(lua_State *L, int offset, uint32 defaultColour) {
	uint32 colour;
	int a, r, g, b;
	uint8 defA, defB, defG, defR;

	LUA_DECOMPOSE_PIXEL(defaultColour, defA, defR, defG, defB);
	defaultColour = (defR << 24) | (defG << 16) | (defB << 8) | defA;

	colour = gui_getcolour_wrapped(L, offset, TRUE, defaultColour);
	a = ((colour & 0xff) * transparencyModifier) / 255;
	if (a > 255) a = 255;
	b = (colour >> 8) & 0xff;
	g = (colour >> 16) & 0xff;
	r = (colour >> 24) & 0xff;
	return LUA_BUILD_PIXEL(a, r, g, b);
}

// gui.drawpixel(x,y,colour)
static int gui_drawpixel(lua_State *L) {

	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L,2);

	uint32 colour = gui_getcolour(L,3);

//	if (!gui_check_boundary(x, y))
//		luaL_error(L,"bad coordinates");

	gui_prepare();

	gui_drawpixel_internal(x, y, colour);

	return 0;
}

// gui.drawline(x1,y1,x2,y2,color,skipFirst)
static int gui_drawline(lua_State *L) {

	int x1,y1,x2,y2;
	uint32 color;
	x1 = luaL_checkinteger(L,1);
	y1 = luaL_checkinteger(L,2);
	x2 = luaL_checkinteger(L,3);
	y2 = luaL_checkinteger(L,4);
	color = gui_optcolour(L,5,LUA_BUILD_PIXEL(255, 255, 255, 255));
	int skipFirst = lua_toboolean(L,6);

	gui_prepare();

	gui_drawline_internal(x2, y2, x1, y1, !skipFirst, color);

	return 0;
}

// gui.drawbox(x1, y1, x2, y2, fillcolor, outlinecolor)
static int gui_drawbox(lua_State *L) {

	int x1,y1,x2,y2;
	uint32 fillcolor;
	uint32 outlinecolor;

	x1 = luaL_checkinteger(L,1);
	y1 = luaL_checkinteger(L,2);
	x2 = luaL_checkinteger(L,3);
	y2 = luaL_checkinteger(L,4);
	fillcolor = gui_optcolour(L,5,LUA_BUILD_PIXEL(63, 255, 255, 255));
	outlinecolor = gui_optcolour(L,6,LUA_BUILD_PIXEL(255, LUA_PIXEL_R(fillcolor), LUA_PIXEL_G(fillcolor), LUA_PIXEL_B(fillcolor)));

	if (x1 > x2) 
		std::swap<int>(x1, x2);
	if (y1 > y2) 
		std::swap<int>(y1, y2);

	gui_prepare();

	gui_drawbox_internal(x1, y1, x2, y2, outlinecolor);
	if ((x2 - x1) >= 2 && (y2 - y1) >= 2)
		gui_fillbox_internal(x1+1, y1+1, x2-1, y2-1, fillcolor);

	return 0;
}

// gui.drawcircle(x0, y0, radius, colour)
//static int gui_drawcircle(lua_State *L) {
//
//	int x, y, r;
//	uint32 colour;
//
//	x = luaL_checkinteger(L,1);
//	y = luaL_checkinteger(L,2);
//	r = luaL_checkinteger(L,3);
//	colour = gui_getcolour(L,4);
//
//	gui_prepare();
//
//	gui_drawcircle_internal(x, y, r, colour);
//
//	return 0;
//}

// gui.fillbox(x1, y1, x2, y2, colour)
//static int gui_fillbox(lua_State *L) {
//
//	int x1,y1,x2,y2;
//	uint32 colour;
//
//	x1 = luaL_checkinteger(L,1);
//	y1 = luaL_checkinteger(L,2);
//	x2 = luaL_checkinteger(L,3);
//	y2 = luaL_checkinteger(L,4);
//	colour = gui_getcolour(L,5);
//
////	if (!gui_check_boundary(x1, y1))
////		luaL_error(L,"bad coordinates");
////
////	if (!gui_check_boundary(x2, y2))
////		luaL_error(L,"bad coordinates");
//
//	gui_prepare();
//
//	gui_fillbox_internal(x1, y1, x2, y2, colour);
//
//	return 0;
//}

// gui.fillcircle(x0, y0, radius, colour)
//static int gui_fillcircle(lua_State *L) {
//
//	int x, y, r;
//	uint32 colour;
//
//	x = luaL_checkinteger(L,1);
//	y = luaL_checkinteger(L,2);
//	r = luaL_checkinteger(L,3);
//	colour = gui_getcolour(L,4);
//
//	gui_prepare();
//
//	gui_fillcircle_internal(x, y, r, colour);
//
//	return 0;
//}


static int gui_getpixel(lua_State *L) {
	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L, 2);

	if(!gui_check_boundary(x,y))
	{
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 0);
	}
	else
	{
		uint8 *screen = (uint8*) XBuf;
		lua_pushinteger(L, screen[y*x*4 + 2]); // red
		lua_pushinteger(L, screen[y*x*4 + 1]); // green
		lua_pushinteger(L, screen[y*x*4 + 0]); // blue
	}

	return 3;
}

static int gui_parsecolor(lua_State *L)
{
	int r, g, b, a;
	uint32 color = gui_getcolour(L,1);
	LUA_DECOMPOSE_PIXEL(color, a, r, g, b);
	lua_pushinteger(L, r);
	lua_pushinteger(L, g);
	lua_pushinteger(L, b);
	lua_pushinteger(L, a);
	return 4;
}


// gui.gdscreenshot()
//
//  Returns a screen shot as a string in gd's v1 file format.
//  This allows us to make screen shots available without gd installed locally.
//  Users can also just grab pixels via substring selection.
//
//  I think...  Does lua support grabbing byte values from a string? // yes, string.byte(str,offset)
//  Well, either way, just install gd and do what you like with it.
//  It really is easier that way.
// example: gd.createFromGdStr(gui.gdscreenshot()):png("outputimage.png")
static int gui_gdscreenshot(lua_State *L) {
	int x,y;

	int width = iScreenWidth;
	int height = iScreenHeight;

	int size = 11 + width * height * 4;
	char* str = (char*)malloc(size+1);
	unsigned char* ptr;
	uint8 *screen;

	str[size] = 0;
	ptr = (unsigned char*)str;

	// GD format header for truecolor image (11 bytes)
	*ptr++ = (65534 >> 8) & 0xFF;
	*ptr++ = (65534     ) & 0xFF;
	*ptr++ = (width >> 8) & 0xFF;
	*ptr++ = (width     ) & 0xFF;
	*ptr++ = (height >> 8) & 0xFF;
	*ptr++ = (height     ) & 0xFF;
	*ptr++ = 1;
	*ptr++ = 255;
	*ptr++ = 255;
	*ptr++ = 255;
	*ptr++ = 255;

	screen=XBuf;
	for(y=0; y<height; y++){
		for(x=0; x<width; x++){
			uint32 r, g, b;
			r = screen[(y*LUA_SCREEN_WIDTH+x)*4+2];
			g = screen[(y*LUA_SCREEN_WIDTH+x)*4+1];
			b = screen[(y*LUA_SCREEN_WIDTH+x)*4];

			// overlay uncommited Lua drawings if needed
			if (gui_used != GUI_CLEAR && gui_enabled) {
				const uint8 gui_alpha = gui_data[(y*LUA_SCREEN_WIDTH+x)*4+3];
				const uint8 gui_red   = gui_data[(y*LUA_SCREEN_WIDTH+x)*4+2];
				const uint8 gui_green = gui_data[(y*LUA_SCREEN_WIDTH+x)*4+1];
				const uint8 gui_blue  = gui_data[(y*LUA_SCREEN_WIDTH+x)*4];

				if (gui_alpha == 255) {
					// direct copy
					r = gui_red;
					g = gui_green;
					b = gui_blue;
				}
				else if (gui_alpha != 0) {
					// alpha-blending
					r = (((int) gui_red   - r) * gui_alpha / 255 + r) & 255;
					g = (((int) gui_green - g) * gui_alpha / 255 + g) & 255;
					b = (((int) gui_blue  - b) * gui_alpha / 255 + b) & 255;
				}
			}

			*ptr++ = 0;
			*ptr++ = r;
			*ptr++ = g;
			*ptr++ = b;
		}
	}

	lua_pushlstring(L, str, size);
	free(str);
	return 1;
}


// gui.opacity(number alphaValue)
// sets the transparency of subsequent draw calls
// 0.0 is completely transparent, 1.0 is completely opaque
// non-integer values are supported and meaningful, as are values greater than 1.0
// it is not necessary to use this function to get transparency (or the less-recommended gui.transparency() either),
// because you can provide an alpha value in the color argument of each draw call.
// however, it can be convenient to be able to globally modify the drawing transparency
static int gui_setopacity(lua_State *L) {
	double opacF = luaL_checknumber(L,1);
	transparencyModifier = (int) (opacF * 255);
	if (transparencyModifier < 0)
		transparencyModifier = 0;
	return 0;
}

// gui.transparency(int strength)
//
//  0 = solid, 
static int gui_transparency(lua_State *L) {
	double trans = luaL_checknumber(L,1);
	transparencyModifier = (int) ((4.0 - trans) / 4.0 * 255);
	if (transparencyModifier < 0)
		transparencyModifier = 0;
	return 0;
}

// gui.clearuncommitted()
//
//  undoes uncommitted drawing commands
static int gui_clearuncommitted(lua_State *L) {
	PCSX_LuaClearGui();
	return 0;
}


static const uint32 Small_Font_Data[] =
{
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,			// 32	 
	0x00000000, 0x00000300, 0x00000400, 0x00000500, 0x00000000, 0x00000700, 0x00000000,			// 33	!
	0x00000000, 0x00040002, 0x00050003, 0x00000000, 0x00000000, 0x00000000, 0x00000000,			// 34	"
	0x00000000, 0x00040002, 0x00050403, 0x00060004, 0x00070605, 0x00080006, 0x00000000,			// 35	#
	0x00000000, 0x00040300, 0x00000403, 0x00000500, 0x00070600, 0x00000706, 0x00000000,			// 36	$
	0x00000000, 0x00000002, 0x00050000, 0x00000500, 0x00000005, 0x00080000, 0x00000000,			// 37	%
	0x00000000, 0x00000300, 0x00050003, 0x00000500, 0x00070005, 0x00080700, 0x00000000,			// 38	&
	0x00000000, 0x00000300, 0x00000400, 0x00000000, 0x00000000, 0x00000000, 0x00000000,			// 39	'
	0x00000000, 0x00000300, 0x00000003, 0x00000004, 0x00000005, 0x00000700, 0x00000000,			// 40	(
	0x00000000, 0x00000300, 0x00050000, 0x00060000, 0x00070000, 0x00000700, 0x00000000,			// 41	)
	0x00000000, 0x00000000, 0x00000400, 0x00060504, 0x00000600, 0x00080006, 0x00000000,			// 42	*
	0x00000000, 0x00000000, 0x00000400, 0x00060504, 0x00000600, 0x00000000, 0x00000000,			// 43	+
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000600, 0x00000700, 0x00000007,			// 44	,
	0x00000000, 0x00000000, 0x00000000, 0x00060504, 0x00000000, 0x00000000, 0x00000000,			// 45	-
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000700, 0x00000000,			// 46	.
	0x00030000, 0x00040000, 0x00000400, 0x00000500, 0x00000005, 0x00000006, 0x00000000,			// 47	/
	0x00000000, 0x00000300, 0x00050003, 0x00060004, 0x00070005, 0x00000700, 0x00000000,			// 48	0
	0x00000000, 0x00000300, 0x00000403, 0x00000500, 0x00000600, 0x00000700, 0x00000000,			// 49	1
	0x00000000, 0x00000302, 0x00050000, 0x00000500, 0x00000005, 0x00080706, 0x00000000,			// 50	2
	0x00000000, 0x00000302, 0x00050000, 0x00000504, 0x00070000, 0x00000706, 0x00000000,			// 51	3
	0x00000000, 0x00000300, 0x00000003, 0x00060004, 0x00070605, 0x00080000, 0x00000000,			// 52	4
	0x00000000, 0x00040302, 0x00000003, 0x00000504, 0x00070000, 0x00000706, 0x00000000,			// 53	5
	0x00000000, 0x00000300, 0x00000003, 0x00000504, 0x00070005, 0x00000700, 0x00000000,			// 54	6
	0x00000000, 0x00040302, 0x00050000, 0x00000500, 0x00000600, 0x00000700, 0x00000000,			// 55	7
	0x00000000, 0x00000300, 0x00050003, 0x00000500, 0x00070005, 0x00000700, 0x00000000,			// 56	8
	0x00000000, 0x00000300, 0x00050003, 0x00060500, 0x00070000, 0x00000700, 0x00000000,			// 57	9
	0x00000000, 0x00000000, 0x00000400, 0x00000000, 0x00000000, 0x00000700, 0x00000000,			// 58	:
	0x00000000, 0x00000000, 0x00000000, 0x00000500, 0x00000000, 0x00000700, 0x00000007,			// 59	;
	0x00000000, 0x00040000, 0x00000400, 0x00000004, 0x00000600, 0x00080000, 0x00000000,			// 60	<
	0x00000000, 0x00000000, 0x00050403, 0x00000000, 0x00070605, 0x00000000, 0x00000000,			// 61	=
	0x00000000, 0x00000002, 0x00000400, 0x00060000, 0x00000600, 0x00000006, 0x00000000,			// 62	>
	0x00000000, 0x00000302, 0x00050000, 0x00000500, 0x00000000, 0x00000700, 0x00000000,			// 63	?
	0x00000000, 0x00000300, 0x00050400, 0x00060004, 0x00070600, 0x00000000, 0x00000000,			// 64	@
	0x00000000, 0x00000300, 0x00050003, 0x00060504, 0x00070005, 0x00080006, 0x00000000,			// 65	A
	0x00000000, 0x00000302, 0x00050003, 0x00000504, 0x00070005, 0x00000706, 0x00000000,			// 66	B
	0x00000000, 0x00040300, 0x00000003, 0x00000004, 0x00000005, 0x00080700, 0x00000000,			// 67	C
	0x00000000, 0x00000302, 0x00050003, 0x00060004, 0x00070005, 0x00000706, 0x00000000,			// 68	D
	0x00000000, 0x00040302, 0x00000003, 0x00000504, 0x00000005, 0x00080706, 0x00000000,			// 69	E
	0x00000000, 0x00040302, 0x00000003, 0x00000504, 0x00000005, 0x00000006, 0x00000000,			// 70	F
	0x00000000, 0x00040300, 0x00000003, 0x00060004, 0x00070005, 0x00080700, 0x00000000,			// 71	G
	0x00000000, 0x00040002, 0x00050003, 0x00060504, 0x00070005, 0x00080006, 0x00000000,			// 72	H
	0x00000000, 0x00000300, 0x00000400, 0x00000500, 0x00000600, 0x00000700, 0x00000000,			// 73	I
	0x00000000, 0x00040000, 0x00050000, 0x00060000, 0x00070005, 0x00000700, 0x00000000,			// 74	J
	0x00000000, 0x00040002, 0x00050003, 0x00000504, 0x00070005, 0x00080006, 0x00000000,			// 75	K
	0x00000000, 0x00000002, 0x00000003, 0x00000004, 0x00000005, 0x00080706, 0x00000000,			// 76	l
	0x00000000, 0x00040002, 0x00050403, 0x00060004, 0x00070005, 0x00080006, 0x00000000,			// 77	M
	0x00000000, 0x00000302, 0x00050003, 0x00060004, 0x00070005, 0x00080006, 0x00000000,			// 78	N
	0x00000000, 0x00040302, 0x00050003, 0x00060004, 0x00070005, 0x00080706, 0x00000000,			// 79	O
	0x00000000, 0x00000302, 0x00050003, 0x00000504, 0x00000005, 0x00000006, 0x00000000,			// 80	P
	0x00000000, 0x00040302, 0x00050003, 0x00060004, 0x00070005, 0x00080706, 0x00090000,			// 81	Q
	0x00000000, 0x00000302, 0x00050003, 0x00000504, 0x00070005, 0x00080006, 0x00000000,			// 82	R
	0x00000000, 0x00040300, 0x00000003, 0x00000500, 0x00070000, 0x00000706, 0x00000000,			// 83	S
	0x00000000, 0x00040302, 0x00000400, 0x00000500, 0x00000600, 0x00000700, 0x00000000,			// 84	T
	0x00000000, 0x00040002, 0x00050003, 0x00060004, 0x00070005, 0x00080706, 0x00000000,			// 85	U
	0x00000000, 0x00040002, 0x00050003, 0x00060004, 0x00000600, 0x00000700, 0x00000000,			// 86	V
	0x00000000, 0x00040002, 0x00050003, 0x00060004, 0x00070605, 0x00080006, 0x00000000,			// 87	W
	0x00000000, 0x00040002, 0x00050003, 0x00000500, 0x00070005, 0x00080006, 0x00000000,			// 88	X
	0x00000000, 0x00040002, 0x00050003, 0x00000500, 0x00000600, 0x00000700, 0x00000000,			// 89	Y
	0x00000000, 0x00040302, 0x00050000, 0x00000500, 0x00000005, 0x00080706, 0x00000000,			// 90	Z
	0x00000000, 0x00040300, 0x00000400, 0x00000500, 0x00000600, 0x00080700, 0x00000000,			// 91	[
	0x00000000, 0x00000002, 0x00000400, 0x00000500, 0x00070000, 0x00080000, 0x00000000,			// 92	'\'
	0x00000000, 0x00000302, 0x00000400, 0x00000500, 0x00000600, 0x00000706, 0x00000000,			// 93	]
	0x00000000, 0x00000300, 0x00050003, 0x00000000, 0x00000000, 0x00000000, 0x00000000,			// 94	^
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00080706, 0x00000000,			// 95	_
	0x00000000, 0x00000002, 0x00000400, 0x00000000, 0x00000000, 0x00000000, 0x00000000,			// 96	`
	0x00000000, 0x00000000, 0x00050400, 0x00060004, 0x00070005, 0x00080700, 0x00000000,			// 97	a
	0x00000000, 0x00000002, 0x00000003, 0x00000504, 0x00070005, 0x00000706, 0x00000000,			// 98	b
	0x00000000, 0x00000000, 0x00050400, 0x00000004, 0x00000005, 0x00080700, 0x00000000,			// 99	c
	0x00000000, 0x00040000, 0x00050000, 0x00060500, 0x00070005, 0x00080700, 0x00000000,			// 100	d
	0x00000000, 0x00000000, 0x00050400, 0x00060504, 0x00000005, 0x00080700, 0x00000000,			// 101	e
	0x00000000, 0x00040300, 0x00000003, 0x00000504, 0x00000005, 0x00000006, 0x00000000,			// 102	f
	0x00000000, 0x00000000, 0x00050400, 0x00060004, 0x00070600, 0x00080000, 0x00000807,			// 103	g
	0x00000000, 0x00000002, 0x00000003, 0x00000504, 0x00070005, 0x00080006, 0x00000000,			// 104	h
	0x00000000, 0x00000300, 0x00000000, 0x00000500, 0x00000600, 0x00000700, 0x00000000,			// 105	i
	0x00000000, 0x00000300, 0x00000000, 0x00000500, 0x00000600, 0x00000700, 0x00000007,			// 106	j
	0x00000000, 0x00000002, 0x00000003, 0x00060004, 0x00000605, 0x00080006, 0x00000000,			// 107	k
	0x00000000, 0x00000300, 0x00000400, 0x00000500, 0x00000600, 0x00080000, 0x00000000,			// 108	l
	0x00000000, 0x00000000, 0x00050003, 0x00060504, 0x00070005, 0x00080006, 0x00000000,			// 109	m
	0x00000000, 0x00000000, 0x00000403, 0x00060004, 0x00070005, 0x00080006, 0x00000000,			// 110	n
	0x00000000, 0x00000000, 0x00000400, 0x00060004, 0x00070005, 0x00000700, 0x00000000,			// 111	o
	0x00000000, 0x00000000, 0x00000400, 0x00060004, 0x00000605, 0x00000006, 0x00000007,			// 112	p
	0x00000000, 0x00000000, 0x00000400, 0x00060004, 0x00070600, 0x00080000, 0x00090000,			// 113	q
	0x00000000, 0x00000000, 0x00050003, 0x00000504, 0x00000005, 0x00000006, 0x00000000,			// 114	r
	0x00000000, 0x00000000, 0x00050400, 0x00000004, 0x00070600, 0x00000706, 0x00000000,			// 115	s
	0x00000000, 0x00000300, 0x00050403, 0x00000500, 0x00000600, 0x00080000, 0x00000000,			// 116	t
	0x00000000, 0x00000000, 0x00050003, 0x00060004, 0x00070005, 0x00080700, 0x00000000,			// 117	u
	0x00000000, 0x00000000, 0x00050003, 0x00060004, 0x00070005, 0x00000700, 0x00000000,			// 118	v
	0x00000000, 0x00000000, 0x00050003, 0x00060004, 0x00070605, 0x00080006, 0x00000000,			// 119	w
	0x00000000, 0x00000000, 0x00050003, 0x00000500, 0x00070005, 0x00080006, 0x00000000,			// 120	x
	0x00000000, 0x00000000, 0x00050003, 0x00060004, 0x00000600, 0x00000700, 0x00000007,			// 121	y
	0x00000000, 0x00000000, 0x00050403, 0x00000500, 0x00000005, 0x00080706, 0x00000000,			// 122	z
	0x00000000, 0x00040300, 0x00000400, 0x00000504, 0x00000600, 0x00080700, 0x00000000,			// 123	{
	0x00000000, 0x00000300, 0x00000400, 0x00000000, 0x00000600, 0x00000700, 0x00000000,			// 124	|
	0x00000000, 0x00000302, 0x00000400, 0x00060500, 0x00000600, 0x00000706, 0x00000000,			// 125	}
	0x00000000, 0x00000302, 0x00050000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,			// 126	~
	0x00000000, 0x00000000, 0x00000400, 0x00060004, 0x00070605, 0x00000000, 0x00000000,			// 127	
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};


static void PutTextInternal (const char *str, int len, short x, short y, int color, int backcolor)
{
	int Opac = (color >> 24) & 0xFF;
	int backOpac = (backcolor >> 24) & 0xFF;
	int origX = x;

	if(!Opac && !backOpac)
		return;

	while(*str && len && y < LUA_SCREEN_HEIGHT)
	{
		int c = *str++;
		const unsigned char* Cur_Glyph;
		int y2,x2,y3,x3;

		while (x > LUA_SCREEN_WIDTH && c != '\n') {
			c = *str;
			if (c == '\0')
				break;
			str++;
		}
		if(c == '\n')
		{
			x = origX;
			y += 8;
			continue;
		}
		else if(c == '\t') // just in case
		{
			const int tabSpace = 8;
			x += (tabSpace-(((x-origX)/4)%tabSpace))*4;
			continue;
		}
		if((unsigned int)(c-32) >= 96)
			continue;
		Cur_Glyph = (const unsigned char*)&Small_Font_Data + (c-32)*7*4;

		for(y2 = 0; y2 < 8; y2++)
		{
			unsigned int glyphLine = *((unsigned int*)Cur_Glyph + y2);
			for(x2 = -1; x2 < 4; x2++)
			{
				int shift = x2 << 3;
				int mask = 0xFF << shift;
				int intensity = (glyphLine & mask) >> shift;

				if(intensity && x2 >= 0 && y2 < 7)
				{
					//int xdraw = max(0,min(LUA_SCREEN_WIDTH - 1,x+x2));
					//int ydraw = max(0,min(LUA_SCREEN_HEIGHT - 1,y+y2));
					//gui_drawpixel_fast(xdraw, ydraw, color);
					gui_drawpixel_internal(x+x2, y+y2, color);
				}
				else if(backOpac)
				{
					for(y3 = max(0,y2-1); y3 <= min(6,y2+1); y3++)
					{
						unsigned int glyphLine = *((unsigned int*)Cur_Glyph + y3);
						for(x3 = max(0,x2-1); x3 <= min(3,x2+1); x3++)
						{
							int shift = x3 << 3;
							int mask = 0xFF << shift;
							intensity |= (glyphLine & mask) >> shift;
							if (intensity)
								goto draw_outline; // speedup?
						}
					}
draw_outline:
					if(intensity)
					{
						//int xdraw = max(0,min(LUA_SCREEN_WIDTH - 1,x+x2));
						//int ydraw = max(0,min(LUA_SCREEN_HEIGHT - 1,y+y2));
						//gui_drawpixel_fast(xdraw, ydraw, backcolor);
						gui_drawpixel_internal(x+x2, y+y2, backcolor);
					}
				}
			}
		}

		x += 4;
		len--;
	}
}


static void LuaDisplayString (const char *string, int y, int x, uint32 color, uint32 outlineColor)
{
	if(!string)
		return;

	gui_prepare();

	PutTextInternal(string, strlen(string), x, y, color, outlineColor);
}


// gui.text(int x, int y, string msg[, color="white"[, outline="black"]])
//
//  Displays the given text on the screen, using the same font and techniques as the
//  main HUD.
static int gui_text(lua_State *L) {
	const char *msg;
	int x, y;
	uint32 colour, borderColour;
	int argCount = lua_gettop(L);

	x = luaL_checkinteger(L,1);
	y = luaL_checkinteger(L,2);
	msg = luaL_checkstring(L,3);

	if(argCount>=4)
		colour = gui_getcolour(L,4);
	else
		colour = gui_optcolour(L,4,LUA_BUILD_PIXEL(255, 255, 255, 255));

	if(argCount>=5)
		borderColour = gui_getcolour(L,5);
	else
		borderColour = gui_optcolour(L,5,LUA_BUILD_PIXEL(255, 0, 0, 0));

	gui_prepare();

	LuaDisplayString(msg, y, x, colour, borderColour);

	return 0;
}


// gui.gdoverlay([int dx=0, int dy=0,] string str [, sx=0, sy=0, sw, sh] [, float alphamul=1.0])
//
//  Overlays the given image on the screen.
// example: gui.gdoverlay(gd.createFromPng("myimage.png"):gdStr())
static int gui_gdoverlay(lua_State *L) {
	int i,y,x;
	int argCount = lua_gettop(L);

	int xStartDst = 0;
	int yStartDst = 0;
	int xStartSrc = 0;
	int yStartSrc = 0;

	const unsigned char* ptr;

	int trueColor;
	int imgwidth;
	int width;
	int imgheight;
	int height;
	int pitch;
	int alphaMul;
	int opacMap[256];
	int colorsTotal = 0;
	int transparent;
	struct { uint8 r, g, b, a; } pal[256];
	const uint8* pix;
	int bytesToNextLine;

	int index = 1;
	if(lua_type(L,index) == LUA_TNUMBER)
	{
		xStartDst = lua_tointeger(L,index++);
		if(lua_type(L,index) == LUA_TNUMBER)
			yStartDst = lua_tointeger(L,index++);
	}

	luaL_checktype(L,index,LUA_TSTRING);
	ptr = (const unsigned char*)lua_tostring(L,index++);

	if (ptr[0] != 255 || (ptr[1] != 254 && ptr[1] != 255))
		luaL_error(L, "bad image data");
	trueColor = (ptr[1] == 254);
	ptr += 2;
	imgwidth = *ptr++ << 8;
	imgwidth |= *ptr++;
	width = imgwidth;
	imgheight = *ptr++ << 8;
	imgheight |= *ptr++;
	height = imgheight;
	if ((!trueColor && *ptr) || (trueColor && !*ptr))
		luaL_error(L, "bad image data");
	ptr++;
	pitch = imgwidth * (trueColor?4:1);

	if ((argCount - index + 1) >= 4) {
		xStartSrc = luaL_checkinteger(L,index++);
		yStartSrc = luaL_checkinteger(L,index++);
		width = luaL_checkinteger(L,index++);
		height = luaL_checkinteger(L,index++);
	}

	alphaMul = transparencyModifier;
	if(lua_isnumber(L, index))
		alphaMul = (int)(alphaMul * lua_tonumber(L, index++));
	if(alphaMul <= 0)
		return 0;

	// since there aren't that many possible opacity levels,
	// do the opacity modification calculations beforehand instead of per pixel
	for(i = 0; i < 128; i++)
	{
		int opac = 255 - ((i << 1) | (i & 1)); // gdAlphaMax = 127, not 255
		opac = (opac * alphaMul) / 255;
		if(opac < 0) opac = 0;
		if(opac > 255) opac = 255;
		opacMap[i] = opac;
	}
	for(i = 128; i < 256; i++)
		opacMap[i] = 0; // what should we do for them, actually?

	if (!trueColor) {
		colorsTotal = *ptr++ << 8;
		colorsTotal |= *ptr++;
	}
	transparent = *ptr++ << 24;
	transparent |= *ptr++ << 16;
	transparent |= *ptr++ << 8;
	transparent |= *ptr++;
	if (!trueColor) for (i = 0; i < 256; i++) {
		pal[i].r = *ptr++;
		pal[i].g = *ptr++;
		pal[i].b = *ptr++;
		pal[i].a = opacMap[*ptr++];
	}

	// some of clippings
	if (xStartSrc < 0) {
		width += xStartSrc;
		xStartDst -= xStartSrc;
		xStartSrc = 0;
	}
	if (yStartSrc < 0) {
		height += yStartSrc;
		yStartDst -= yStartSrc;
		yStartSrc = 0;
	}
	if (xStartSrc+width >= imgwidth)
		width = imgwidth - xStartSrc;
	if (yStartSrc+height >= imgheight)
		height = imgheight - yStartSrc;
	if (xStartDst < 0) {
		width += xStartDst;
		if (width <= 0)
			return 0;
		xStartSrc = -xStartDst;
		xStartDst = 0;
	}
	if (yStartDst < 0) {
		height += yStartDst;
		if (height <= 0)
			return 0;
		yStartSrc = -yStartDst;
		yStartDst = 0;
	}
	if (xStartDst+width >= LUA_SCREEN_WIDTH)
		width = LUA_SCREEN_WIDTH - xStartDst;
	if (yStartDst+height >= LUA_SCREEN_HEIGHT)
		height = LUA_SCREEN_HEIGHT - yStartDst;
	if (width <= 0 || height <= 0)
		return 0; // out of screen or invalid size

	gui_prepare();

	pix = (const uint8*)(&ptr[yStartSrc*pitch + (xStartSrc*(trueColor?4:1))]);
	bytesToNextLine = pitch - (width * (trueColor?4:1));
	if (trueColor)
		for (y = yStartDst; y < height+yStartDst && y < LUA_SCREEN_HEIGHT; y++, pix += bytesToNextLine) {
			for (x = xStartDst; x < width+xStartDst && x < LUA_SCREEN_WIDTH; x++, pix += 4) {
				gui_drawpixel_fast(x, y, LUA_BUILD_PIXEL(opacMap[pix[0]], pix[1], pix[2], pix[3]));
			}
		}
	else
		for (y = yStartDst; y < height+yStartDst && y < LUA_SCREEN_HEIGHT; y++, pix += bytesToNextLine) {
			for (x = xStartDst; x < width+xStartDst && x < LUA_SCREEN_WIDTH; x++, pix++) {
				gui_drawpixel_fast(x, y, LUA_BUILD_PIXEL(pal[*pix].a, pal[*pix].r, pal[*pix].g, pal[*pix].b));
			}
		}

	return 0;
}


// function gui.register(function f)
//
//  This function will be called just before a graphical update.
//  More complicated, but doesn't suffer any frame delays.
//  Nil will be accepted in place of a function to erase
//  a previously registered function, and the previous function
//  (if any) is returned, or nil if none.
static int gui_register(lua_State *L) {
	// We'll do this straight up.

	// First set up the stack.
	lua_settop(L,1);
	
	// Verify the validity of the entry
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);

	// Get the old value
	lua_getfield(L, LUA_REGISTRYINDEX, guiCallbackTable);
	
	// Save the new value
	lua_pushvalue(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, guiCallbackTable);
	
	// The old value is on top of the stack. Return it.
	return 1;
}


static int doPopup(lua_State *L, const char* deftype, const char* deficon) {
	const char *str = luaL_checkstring(L, 1);
	const char* type = lua_type(L,2) == LUA_TSTRING ? lua_tostring(L,2) : deftype;
	const char* icon = lua_type(L,3) == LUA_TSTRING ? lua_tostring(L,3) : deficon;

	int itype = -1, iters = 0;
	int iicon = -1;
	static const char * const titles [] = {"Notice", "Question", "Warning", "Error"};
	const char* answer = "ok";

	while(itype == -1 && iters++ < 2)
	{
		if(!stricmp(type, "ok")) itype = 0;
		else if(!stricmp(type, "yesno")) itype = 1;
		else if(!stricmp(type, "yesnocancel")) itype = 2;
		else if(!stricmp(type, "okcancel")) itype = 3;
		else if(!stricmp(type, "abortretryignore")) itype = 4;
		else type = deftype;
	}
	assert(itype >= 0 && itype <= 4);
	if(!(itype >= 0 && itype <= 4)) itype = 0;

	iters = 0;
	while(iicon == -1 && iters++ < 2)
	{
		if(!stricmp(icon, "message") || !stricmp(icon, "notice")) iicon = 0;
		else if(!stricmp(icon, "question")) iicon = 1;
		else if(!stricmp(icon, "warning")) iicon = 2;
		else if(!stricmp(icon, "error")) iicon = 3;
		else icon = deficon;
	}
	assert(iicon >= 0 && iicon <= 3);
	if(!(iicon >= 0 && iicon <= 3)) iicon = 0;

#ifdef WIN32
	{
		static const int etypes [] = {MB_OK, MB_YESNO, MB_YESNOCANCEL, MB_OKCANCEL, MB_ABORTRETRYIGNORE};
		static const int eicons [] = {MB_ICONINFORMATION, MB_ICONQUESTION, MB_ICONWARNING, MB_ICONERROR};
		int ianswer = MessageBox(gApp.hWnd, str, titles[iicon], etypes[itype] | eicons[iicon]);
		switch(ianswer)
		{
			case IDOK: answer = "ok"; break;
			case IDCANCEL: answer = "cancel"; break;
			case IDABORT: answer = "abort"; break;
			case IDRETRY: answer = "retry"; break;
			case IDIGNORE: answer = "ignore"; break;
			case IDYES: answer = "yes"; break;
			case IDNO: answer = "no"; break;
		}

		lua_pushstring(L, answer);
		return 1;
	}
#else

	char *t;
#ifdef __linux
	int pid; // appease compiler

	// Before doing any work, verify the correctness of the parameters.
	if (strcmp(type, "ok") == 0)
		t = "OK:100";
	else if (strcmp(type, "yesno") == 0)
		t = "Yes:100,No:101";
	else if (strcmp(type, "yesnocancel") == 0)
		t = "Yes:100,No:101,Cancel:102";
	else
		return luaL_error(L, "invalid popup type \"%s\"", type);

	// Can we find a copy of xmessage? Search the path.
	
	char *path = strdup(getenv("PATH"));

	char *current = path;
	
	char *colon;

	int found = 0;

	while (current) {
		colon = strchr(current, ':');
		
		// Clip off the colon.
		*colon++ = 0;
		
		int len = strlen(current);
		char *filename = (char*)malloc(len + 12); // always give excess
		snprintf(filename, len+12, "%s/xmessage", current);
		
		if (access(filename, X_OK) == 0) {
			free(filename);
			found = 1;
			break;
		}
		
		// Failed, move on.
		current = colon;
		free(filename);
		
	}

	free(path);

	// We've found it?
	if (!found)
		goto use_console;

	pid = fork();
	if (pid == 0) {// I'm the virgin sacrifice
	
		// I'm gonna be dead in a matter of microseconds anyways, so wasted memory doesn't matter to me.
		// Go ahead and abuse strdup.
		char * parameters[] = {"xmessage", "-buttons", t, strdup(str), NULL};

		execvp("xmessage", parameters);
		
		// Aw shitty
		perror("exec xmessage");
		exit(1);
	}
	else if (pid < 0) // something went wrong!!! Oh hell... use the console
		goto use_console;
	else {
		// We're the parent. Watch for the child.
		int r;
		int res = waitpid(pid, &r, 0);
		if (res < 0) // wtf?
			goto use_console;
		
		// The return value gets copmlicated...
		if (!WIFEXITED(r)) {
			luaL_error(L, "don't screw with my xmessage process!");
		}
		r = WEXITSTATUS(r);
		
		// We assume it's worked.
		if (r == 0)
		{
			return 0; // no parameters for an OK
		}
		if (r == 100) {
			lua_pushstring(L, "yes");
			return 1;
		}
		if (r == 101) {
			lua_pushstring(L, "no");
			return 1;
		}
		if (r == 102) {
			lua_pushstring(L, "cancel");
			return 1;
		}
		
		// Wtf?
		return luaL_error(L, "popup failed due to unknown results involving xmessage (%d)", r);
	}

use_console:
#endif

	// All else has failed

	if (strcmp(type, "ok") == 0)
		t = "";
	else if (strcmp(type, "yesno") == 0)
		t = "yn";
	else if (strcmp(type, "yesnocancel") == 0)
		t = "ync";
	else
		return luaL_error(L, "invalid popup type \"%s\"", type);

	fprintf(stderr, "Lua Message: %s\n", str);

	while (true) {
		char buffer[64];

		// We don't want parameters
		if (!t[0]) {
			fprintf(stderr, "[Press Enter]");
			fgets(buffer, sizeof(buffer), stdin);
			// We're done
			return 0;

		}
		fprintf(stderr, "(%s): ", t);
		fgets(buffer, sizeof(buffer), stdin);
		
		// Check if the option is in the list
		if (strchr(t, tolower(buffer[0]))) {
			switch (tolower(buffer[0])) {
			case 'y':
				lua_pushstring(L, "yes");
				return 1;
			case 'n':
				lua_pushstring(L, "no");
				return 1;
			case 'c':
				lua_pushstring(L, "cancel");
				return 1;
			default:
				luaL_error(L, "internal logic error in console based prompts for gui.popup");
			
			}
		}
		
		// We fell through, so we assume the user answered wrong and prompt again.
	
	}

	// Nothing here, since the only way out is in the loop.
#endif

}

// string gui.popup(string message, string type = "ok", string icon = "message")
// string input.popup(string message, string type = "yesno", string icon = "question")
static int gui_popup(lua_State *L)
{
	return doPopup(L, "ok", "message");
}
static int input_popup(lua_State *L)
{
	return doPopup(L, "yesno", "question");
}

#ifdef WIN32

const char* s_keyToName[256] =
{
	NULL,
	"leftclick",
	"rightclick",
	NULL,
	"middleclick",
	NULL,
	NULL,
	NULL,
	"backspace",
	"tab",
	NULL,
	NULL,
	NULL,
	"enter",
	NULL,
	NULL,
	"shift", // 0x10
	"control",
	"alt",
	"pause",
	"capslock",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"escape",
	NULL,
	NULL,
	NULL,
	NULL,
	"space", // 0x20
	"pageup",
	"pagedown",
	"end",
	"home",
	"left",
	"up",
	"right",
	"down",
	NULL,
	NULL,
	NULL,
	NULL,
	"insert",
	"delete",
	NULL,
	"0","1","2","3","4","5","6","7","8","9",
	NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	"A","B","C","D","E","F","G","H","I","J",
	"K","L","M","N","O","P","Q","R","S","T",
	"U","V","W","X","Y","Z",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"numpad0","numpad1","numpad2","numpad3","numpad4","numpad5","numpad6","numpad7","numpad8","numpad9",
	"numpad*","numpad+",
	NULL,
	"numpad-","numpad.","numpad/",
	"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
	"F13","F14","F15","F16","F17","F18","F19","F20","F21","F22","F23","F24",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"numlock",
	"scrolllock",
	NULL, // 0x92
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, // 0xB9
	"semicolon",
	"plus",
	"comma",
	"minus",
	"period",
	"slash",
	"tilde",
	NULL, // 0xC1
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, // 0xDA
	"leftbracket",
	"backslash",
	"rightbracket",
	"quote",
};

void GetMouseData(uint32 *md)
{
	extern uint32 mousex,mousey;
	RECT t;
	GetClientRect(gApp.hWnd, &t);
	md[0] = (uint32)(mousex / ((float)t.right / iScreenWidth));
	md[1] = (uint32)(mousey / ((float)t.bottom / iScreenHeight));
}

#endif

// input.get()
// takes no input, returns a lua table of entries representing the current input state,
// independent of the joypad buttons the emulated game thinks are pressed
// for example:
//   if the user is holding the W key and the left mouse button
//   and has the mouse at the bottom-right corner of the game screen,
//   then this would return {W=true, leftclick=true, xmouse=255, ymouse=223}
static int input_getcurrentinputstatus(lua_State *L) {
	lua_newtable(L);

#ifdef WIN32
	// keyboard and mouse button status
	{
		int i;
		for(i = 1; i < 255; i++) {
			const char* name = s_keyToName[i];
			if(name) {
				int active;
				if(i == VK_CAPITAL || i == VK_NUMLOCK || i == VK_SCROLL)
					active = GetKeyState(i) & 0x01;
				else
					active = GetAsyncKeyState(i) & 0x8000;
				if(active) {
					lua_pushboolean(L, TRUE);
					lua_setfield(L, -2, name);
				}
			}
		}
	}
	// mouse position in game screen pixel coordinates
	{
		uint32 MouseData[2];
		int x, y;
		GetMouseData(MouseData);
		x = MouseData[0];
		y = MouseData[1];
	
		lua_pushinteger(L, x);
		lua_setfield(L, -2, "xmouse");
		lua_pushinteger(L, y);
		lua_setfield(L, -2, "ymouse");
	}
#else
	// NYI (well, return an empty table)
#endif

	return 1;
}


// the following bit operations are ported from LuaBitOp 1.0.1,
// because it can handle the sign bit (bit 31) correctly.

/*
** Lua BitOp -- a bit operations library for Lua 5.1.
** http://bitop.luajit.org/
**
** Copyright (C) 2008-2009 Mike Pall. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining
** a copy of this software and associated documentation files (the
** "Software"), to deal in the Software without restriction, including
** without limitation the rights to use, copy, modify, merge, publish,
** distribute, sublicense, and/or sell copies of the Software, and to
** permit persons to whom the Software is furnished to do so, subject to
** the following conditions:
**
** The above copyright notice and this permission notice shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
** CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
** SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
** [ MIT license: http://www.opensource.org/licenses/mit-license.php ]
*/

#ifdef _MSC_VER
/* MSVC is stuck in the last century and doesn't have C99's stdint.h. */
typedef __int32 int32_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
#else
#include <stdint.h>
#endif

typedef int32_t SBits;
typedef uint32_t UBits;

typedef union {
  lua_Number n;
#ifdef LUA_NUMBER_DOUBLE
  uint64_t b;
#else
  UBits b;
#endif
} BitNum;

/* Convert argument to bit type. */
static UBits barg(lua_State *L, int idx)
{
  BitNum bn;
  UBits b;
  bn.n = lua_tonumber(L, idx);
#if defined(LUA_NUMBER_DOUBLE)
  bn.n += 6755399441055744.0;  /* 2^52+2^51 */
#ifdef SWAPPED_DOUBLE
  b = (UBits)(bn.b >> 32);
#else
  b = (UBits)bn.b;
#endif
#elif defined(LUA_NUMBER_INT) || defined(LUA_NUMBER_LONG) || \
      defined(LUA_NUMBER_LONGLONG) || defined(LUA_NUMBER_LONG_LONG) || \
      defined(LUA_NUMBER_LLONG)
  if (sizeof(UBits) == sizeof(lua_Number))
    b = bn.b;
  else
    b = (UBits)(SBits)bn.n;
#elif defined(LUA_NUMBER_FLOAT)
#error "A 'float' lua_Number type is incompatible with this library"
#else
#error "Unknown number type, check LUA_NUMBER_* in luaconf.h"
#endif
  if (b == 0 && !lua_isnumber(L, idx))
    luaL_typerror(L, idx, "number");
  return b;
}

/* Return bit type. */
#define BRET(b)  lua_pushnumber(L, (lua_Number)(SBits)(b)); return 1;

static int bit_tobit(lua_State *L) { BRET(barg(L, 1)) }
static int bit_bnot(lua_State *L) { BRET(~barg(L, 1)) }

#define BIT_OP(func, opr) \
  static int func(lua_State *L) { int i; UBits b = barg(L, 1); \
    for (i = lua_gettop(L); i > 1; i--) b opr barg(L, i); BRET(b) }
BIT_OP(bit_band, &=)
BIT_OP(bit_bor, |=)
BIT_OP(bit_bxor, ^=)

#define bshl(b, n)  (b << n)
#define bshr(b, n)  (b >> n)
#define bsar(b, n)  ((SBits)b >> n)
#define brol(b, n)  ((b << n) | (b >> (32-n)))
#define bror(b, n)  ((b << (32-n)) | (b >> n))
#define BIT_SH(func, fn) \
  static int func(lua_State *L) { \
    UBits b = barg(L, 1); UBits n = barg(L, 2) & 31; BRET(fn(b, n)) }
BIT_SH(bit_lshift, bshl)
BIT_SH(bit_rshift, bshr)
BIT_SH(bit_arshift, bsar)
BIT_SH(bit_rol, brol)
BIT_SH(bit_ror, bror)

static int bit_bswap(lua_State *L)
{
  UBits b = barg(L, 1);
  b = (b >> 24) | ((b >> 8) & 0xff00) | ((b & 0xff00) << 8) | (b << 24);
  BRET(b)
}

static int bit_tohex(lua_State *L)
{
  UBits b = barg(L, 1);
  SBits n = lua_isnone(L, 2) ? 8 : (SBits)barg(L, 2);
  const char *hexdigits = "0123456789abcdef";
  char buf[8];
  int i;
  if (n < 0) { n = -n; hexdigits = "0123456789ABCDEF"; }
  if (n > 8) n = 8;
  for (i = (int)n; --i >= 0; ) { buf[i] = hexdigits[b & 15]; b >>= 4; }
  lua_pushlstring(L, buf, (size_t)n);
  return 1;
}

static const struct luaL_Reg bit_funcs[] = {
  { "tobit",	bit_tobit },
  { "bnot",	bit_bnot },
  { "band",	bit_band },
  { "bor",	bit_bor },
  { "bxor",	bit_bxor },
  { "lshift",	bit_lshift },
  { "rshift",	bit_rshift },
  { "arshift",	bit_arshift },
  { "rol",	bit_rol },
  { "ror",	bit_ror },
  { "bswap",	bit_bswap },
  { "tohex",	bit_tohex },
  { NULL, NULL }
};

/* Signed right-shifts are implementation-defined per C89/C99.
** But the de facto standard are arithmetic right-shifts on two's
** complement CPUs. This behaviour is required here, so test for it.
*/
#define BAD_SAR		(bsar(-8, 2) != (SBits)-2)

BOOL luabitop_validate(lua_State *L) // originally named as luaopen_bit
{
  UBits b;
  lua_pushnumber(L, (lua_Number)1437217655L);
  b = barg(L, -1);
  if (b != (UBits)1437217655L || BAD_SAR) {  /* Perform a simple self-test. */
    const char *msg = "compiled with incompatible luaconf.h";
#ifdef LUA_NUMBER_DOUBLE
#ifdef WIN32
    if (b == (UBits)1610612736L)
      msg = "use D3DCREATE_FPU_PRESERVE with DirectX";
#endif
    if (b == (UBits)1127743488L)
      msg = "not compiled with SWAPPED_DOUBLE";
#endif
    if (BAD_SAR)
      msg = "arithmetic right-shift broken";
    luaL_error(L, "bit library self-test failed (%s)", msg);
    return FALSE;
  }
  return TRUE;
}

// LuaBitOp ends here

static int bit_bshift_emulua(lua_State *L)
{
	int shift = luaL_checkinteger(L,2);
	if (shift < 0) {
		lua_pushinteger(L, -shift);
		lua_replace(L, 2);
		return bit_lshift(L);
	}
	else
		return bit_rshift(L);
}

static int bitbit(lua_State *L)
{
	int rv = 0;
	int numArgs = lua_gettop(L);
	int i;
	for(i = 1; i <= numArgs; i++) {
		int where = luaL_checkinteger(L,i);
		if (where >= 0 && where < 32)
			rv |= (1 << where);
	}
	lua_settop(L,0);
	BRET(rv);
}


// The function called periodically to ensure Lua doesn't run amok.
static void PCSX_LuaHookFunction(lua_State *L, lua_Debug *dbg) {
	if (numTries-- == 0) {

		int kill = 0;

#ifdef WIN32
		// Uh oh
		int ret = MessageBox(gApp.hWnd, "The Lua script running has been running a long time. It may have gone crazy. Kill it?\n\n(No = don't check anymore either)", "Lua Script Gone Nuts?", MB_YESNO);
		
		if (ret == IDYES) {
			kill = 1;
		}

#else
		fprintf(stderr, "The Lua script running has been running a long time.\nIt may have gone crazy. Kill it? (I won't ask again if you say No)\n");
		char buffer[64];
		while (TRUE) {
			fprintf(stderr, "(y/n): ");
			fgets(buffer, sizeof(buffer), stdin);
			if (buffer[0] == 'y' || buffer[0] == 'Y') {
				kill = 1;
				break;
			}
			
			if (buffer[0] == 'n' || buffer[0] == 'N')
				break;
		}
#endif

		if (kill) {
			luaL_error(L, "Killed by user request.");
			PCSX_LuaOnStop();
		}

		// else, kill the debug hook.
		lua_sethook(L, NULL, 0, 0);
	}
}


void HandleCallbackError(lua_State* L)
{
	if(L->errfunc || L->errorJmp)
		luaL_error(L, "%s", lua_tostring(L,-1));
	else {
		lua_pushnil(LUA);
		lua_setfield(LUA, LUA_REGISTRYINDEX, guiCallbackTable);

		// Error?
#ifdef WIN32
		MessageBox( gApp.hWnd, lua_tostring(LUA,-1), "Lua run error", MB_OK | MB_ICONSTOP);
#else
		fprintf(stderr, "Lua thread bombed out: %s\n", lua_tostring(LUA,-1));
#endif

		PCSX_LuaStop();
	}
}


void CallExitFunction() {
	int errorcode = 0;

	if (!LUA)
		return;

	lua_settop(LUA, 0);
	lua_getfield(LUA, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);

	if (lua_isfunction(LUA, -1))
	{
		chdir(luaCWD);
		errorcode = lua_pcall(LUA, 0, 0, 0);
		_getcwd(luaCWD, _MAX_PATH);
	}

	if (errorcode)
		HandleCallbackError(LUA);
}


void CallRegisteredLuaFunctions(int calltype)
{
	const char* idstring;
	int errorcode = 0;

	assert((unsigned int)calltype < (unsigned int)LUACALL_COUNT);
	idstring = luaCallIDStrings[calltype];

	if (!LUA)
		return;

	lua_settop(LUA, 0);
	lua_getfield(LUA, LUA_REGISTRYINDEX, idstring);

	if (lua_isfunction(LUA, -1))
	{
		errorcode = lua_pcall(LUA, 0, 0, 0);
		if (errorcode)
			HandleCallbackError(LUA);
	}
	else
	{
		lua_pop(LUA, 1);
	}
}


static const struct luaL_reg pcsxlib [] = {
	{"speedmode", pcsx_speedmode},
	{"frameadvance", pcsx_frameadvance},
	{"pause", pcsx_pause},
	{"unpause", pcsx_unpause},
	{"framecount", movie_framecount},
	{"lagcount", pcsx_lagcount},
	{"lagged", pcsx_lagged},
	{"registerbefore", pcsx_registerbefore},
	{"registerafter", pcsx_registerafter},
	{"registerexit", pcsx_registerexit},
	{"message", pcsx_message},
	{"print", print}, // sure, why not
	{NULL,NULL}
};

static const struct luaL_reg memorylib [] = {
	{"readbyte", memory_readbyte},
	{"readbytesigned", memory_readbytesigned},
	{"readword", memory_readword},
	{"readwordsigned", memory_readwordsigned},
	{"readdword", memory_readdword},
	{"readdwordsigned", memory_readdwordsigned},
	{"readbyterange", memory_readbyterange},
	{"writebyte", memory_writebyte},
	{"writeword", memory_writeword},
	{"writedword", memory_writedword},
	// alternate naming scheme for word and double-word and unsigned
	{"readbyteunsigned", memory_readbyte},
	{"readwordunsigned", memory_readword},
	{"readdwordunsigned", memory_readdword},
	{"readshort", memory_readword},
	{"readshortunsigned", memory_readword},
	{"readshortsigned", memory_readwordsigned},
	{"readlong", memory_readdword},
	{"readlongunsigned", memory_readdword},
	{"readlongsigned", memory_readdwordsigned},
	{"writeshort", memory_writeword},
	{"writelong", memory_writedword},

	// memory hooks
	{"registerwrite", memory_registerwrite},
	// alternate names
	{"register", memory_registerwrite},

	{NULL,NULL}
};

static const struct luaL_reg joypadlib[] = {
	{"get", joypad_get},
	{"getdown", joypad_getdown},
	{"getup", joypad_getup},
	{"set", joypad_set},
	// alternative names
	{"read", joypad_get},
	{"write", joypad_set},
	{"readdown", joypad_getdown},
	{"readup", joypad_getup},
	{NULL,NULL}
};

static const struct luaL_reg savestatelib[] = {
	{"create", savestate_create},
	{"save", savestate_save},
	{"load", savestate_load},

	{NULL,NULL}
};

static const struct luaL_reg movielib[] = {

	{"active", movie_isactive},
	{"recording", movie_isrecording},
	{"playing", movie_isplaying},
	{"mode", movie_mode},

	{"length", movie_getlength},
	{"name", movie_getname},
	{"rerecordcount", movie_rerecordcount},
	{"setrerecordcount", movie_setrerecordcount},

	{"framecount", movie_framecount},
	{"rerecordcounting", movie_rerecordcounting},
	{"stop", movie_stop},

	// alternative names
	{"close", movie_stop},
	{NULL,NULL}
};

static const struct luaL_reg guilib[] = {
	{"register", gui_register},
	{"text", gui_text},
	{"box", gui_drawbox},
	{"line", gui_drawline},
	{"pixel", gui_drawpixel},
	{"opacity", gui_setopacity},
	{"transparency", gui_transparency},
	{"popup", gui_popup},
	{"parsecolor", gui_parsecolor},
	{"gdscreenshot", gui_gdscreenshot},
	{"gdoverlay", gui_gdoverlay},
	{"getpixel", gui_getpixel},
	{"clearuncommitted", gui_clearuncommitted},
	// alternative names
	{"drawtext", gui_text},
	{"drawbox", gui_drawbox},
	{"drawline", gui_drawline},
	{"drawpixel", gui_drawpixel},
	{"setpixel", gui_drawpixel},
	{"writepixel", gui_drawpixel},
	{"rect", gui_drawbox},
	{"drawrect", gui_drawbox},
	{"drawimage", gui_gdoverlay},
	{"image", gui_gdoverlay},
	{"readpixel", gui_getpixel},
	{NULL,NULL}
};

static const struct luaL_reg inputlib[] = {
	{"get", input_getcurrentinputstatus},
	{"popup", input_popup},
	// alternative names
	{"read", input_getcurrentinputstatus},
	{NULL, NULL}
};


void PCSX_LuaFrameBoundary() {
	lua_State *thread;
	int result;

	// HA!
	if (!LUA || !luaRunning)
		return;

	// Our function needs calling
	lua_settop(LUA,0);
	lua_getfield(LUA, LUA_REGISTRYINDEX, frameAdvanceThread);
	thread = lua_tothread(LUA,1);	

	// Lua calling C must know that we're busy inside a frame boundary
	frameBoundary = TRUE;
	frameAdvanceWaiting = FALSE;

	lua_joypads_used = 0;

	numTries = 1000;
	chdir(luaCWD);
	result = lua_resume(thread, 0);
	_getcwd(luaCWD, _MAX_PATH);
	
	if (result == LUA_YIELD) {
		// Okay, we're fine with that.
	} else if (result != 0) {
		// Done execution by bad causes
		PCSX_LuaOnStop();
		lua_pushnil(LUA);
		lua_setfield(LUA, LUA_REGISTRYINDEX, frameAdvanceThread);
		
		// Error?
#ifdef WIN32
		MessageBox( gApp.hWnd, lua_tostring(thread,-1), "Lua run error", MB_OK | MB_ICONSTOP);
#else
		fprintf(stderr, "Lua thread bombed out: %s\n", lua_tostring(thread,-1));
#endif

	} else {
		PCSX_LuaOnStop();
		//GPU_displayText("Script died of natural causes.\n");
	}

	// Past here, the nes actually runs, so any Lua code is called mid-frame. We must
	// not do anything too stupid, so let ourselves know.
	frameBoundary = FALSE;

	if (!frameAdvanceWaiting) {
		PCSX_LuaOnStop();
	}
}


/**
 * Loads and runs the given Lua script.
 * The emulator MUST be paused for this function to be
 * called. Otherwise, all frame boundary assumptions go out the window.
 *
 * Returns true on success, false on failure.
 */
int PCSX_LoadLuaCode(const char *filename) {
	lua_State *thread;
	int result;
	char dir[_MAX_PATH];
	char *slash, *backslash;

	usingMemoryRegister=0;

	if (filename != luaScriptName)
	{
		if (luaScriptName) free(luaScriptName);
		luaScriptName = strdup(filename);
	}

	// Set current directory from filename (for dofile)
	strcpy(dir, filename);
	slash = strrchr(dir, '/');
	backslash = strrchr(dir, '\\');
	if (!slash || (backslash && backslash < slash))
		slash = backslash;
	if (slash) {
		slash[1] = '\0';    // keep slash itself for some reasons
		_chdir(dir);
	}
	_getcwd(luaCWD, _MAX_PATH);

	if (!LUA) {
		LUA = lua_open();
		luaL_openlibs(LUA);

		luaL_register(LUA, "emu", pcsxlib);
		luaL_register(LUA, "pcsx", pcsxlib);
		luaL_register(LUA, "memory", memorylib);
		luaL_register(LUA, "joypad", joypadlib);
		luaL_register(LUA, "savestate", savestatelib);
		luaL_register(LUA, "movie", movielib);
		luaL_register(LUA, "gui", guilib);
		luaL_register(LUA, "input", inputlib);
		luaL_register(LUA, "bit", bit_funcs); // LuaBitOp library
		lua_settop(LUA, 0); // clean the stack, because each call to luaL_register leaves a table on top

		// register a few utility functions outside of libraries (in the global namespace)
		lua_register(LUA, "print", print);
		lua_register(LUA, "tostring", tostring);
		lua_register(LUA, "addressof", addressof);
		lua_register(LUA, "copytable", copytable);

		// old bit operation functions
		lua_register(LUA, "AND", bit_band);
		lua_register(LUA, "OR", bit_bor);
		lua_register(LUA, "XOR", bit_bxor);
		lua_register(LUA, "SHIFT", bit_bshift_emulua);
		lua_register(LUA, "BIT", bitbit);

		luabitop_validate(LUA);

		lua_newtable(LUA);
		lua_setfield(LUA, LUA_REGISTRYINDEX, memoryWatchTable);
		lua_newtable(LUA);
		lua_setfield(LUA, LUA_REGISTRYINDEX, memoryValueTable);
	}

	// We make our thread NOW because we want it at the bottom of the stack.
	// If all goes wrong, we let the garbage collector remove it.
	thread = lua_newthread(LUA);
	
	// Load the data	
	result = luaL_loadfile(LUA,filename);

	if (result) {
#ifdef WIN32
		// Doing this here caused nasty problems; reverting to MessageBox-from-dialog behavior.
		MessageBox(NULL, lua_tostring(LUA,-1), "Lua load error", MB_OK | MB_ICONSTOP);
#else
		fprintf(stderr, "Failed to compile file: %s\n", lua_tostring(LUA,-1));
#endif

		// Wipe the stack. Our thread
		lua_settop(LUA,0);
		return 0; // Oh shit.
	}

	
	// Get our function into it
	lua_xmove(LUA, thread, 1);
	
	// Save the thread to the registry. This is why I make the thread FIRST.
	lua_setfield(LUA, LUA_REGISTRYINDEX, frameAdvanceThread);
	

	// Initialize settings
	luaRunning = TRUE;
	skipRerecords = FALSE;
	transparencyModifier = 255; // opaque
	lua_joypads_used = 0; // not used

#ifdef WIN32
	info_print = PrintToWindowConsole;
	info_onstart = WinLuaOnStart;
	info_onstop = WinLuaOnStop;
	if(!LuaConsoleHWnd)
		LuaConsoleHWnd = CreateDialog(gApp.hInstance, MAKEINTRESOURCE(IDD_LUA), NULL, (DLGPROC) DlgLuaScriptDialog);
	info_uid = (int)LuaConsoleHWnd;
#else
	info_print = NULL;
	info_onstart = NULL;
	info_onstop = NULL;
#endif
	if (info_onstart)
		info_onstart(info_uid);

	// And run it right now. :)
	PCSX_LuaFrameBoundary();

	// Set up our protection hook to be executed once every 10,000 bytecode instructions.
	lua_sethook(thread, PCSX_LuaHookFunction, LUA_MASKCOUNT, 10000);

	// We're done.
	return 1;
}


/**
 * Equivalent to repeating the last PCSX_LoadLuaCode() call.
 */
void PCSX_ReloadLuaCode()
{
	if (!luaScriptName)
		GPU_displayText("There's no script to reload.");
	else
		PCSX_LoadLuaCode(luaScriptName);
}


/**
 * Terminates a running Lua script by killing the whole Lua engine.
 *
 * Always safe to call, except from within a lua call itself (duh).
 *
 */
void PCSX_LuaStop() {
	//already killed
	if (!LUA) return;

	//execute the user's shutdown callbacks
	CallExitFunction();

	if (info_onstop)
		info_onstop(info_uid);

	lua_close(LUA); // this invokes our garbage collectors for us
	LUA = NULL;
	PCSX_LuaOnStop();
}


/**
 * Returns true if there is a Lua script running.
 *
 */
int PCSX_LuaRunning() {
	// FIXME: return false when no callback functions are registered.
	return (int) (LUA != NULL); // should return true if callback functions are active.
}


/**
 * Returns true if Lua would like to steal the given joypad control.
 */
int PCSX_LuaUsingJoypad(int which) {
	if (!PCSX_LuaRunning())
		return 0;
	return lua_joypads_used & (1 << which);
}


/**
 * Reads the buttons Lua is feeding for the given joypad, in the same
 * format as the OS-specific code.
 *
 * This function must not be called more than once per frame. Ideally exactly once
 * per frame (if PCSX_LuaUsingJoypad says it's safe to do so)
 */
uint32 PCSX_LuaReadJoypad(int which) {
	if (!PCSX_LuaRunning())
		return 0;
	if (lua_joypads_used & (1 << which)) {
		return lua_joypads[which];
	}
	else
		return 0; // disconnected
}


/**
 * If this function returns true, the movie code should NOT increment
 * the rerecord count for a load-state.
 *
 * This function will not return true if a script is not running.
 */
int PCSX_LuaRerecordCountSkip() {
	// FIXME: return true if (there are any active callback functions && skipRerecords)
	return LUA && luaRunning && skipRerecords;
}


/**
 * Given an 8-bit screen with the indicated resolution,
 * draw the current GUI onto it.
 *
 * Currently we only support 256x* resolutions.
 */
void PCSX_LuaGui(void *s, int width, int height, int bpp, int pitch) {
	int x,y;
	uint8 r,g,b,gui_alpha,gui_red,gui_green,gui_blue;

	XBuf = (uint8 *)s;
	iScreenWidth = width;
	iScreenHeight = height;
	if (pitch >=3) {
		LUA_SCREEN_WIDTH  = 1024;
		LUA_SCREEN_HEIGHT = 1024;
	}
	else {
		LUA_SCREEN_WIDTH  = 640;
		LUA_SCREEN_HEIGHT = 512;
	}

	if (!LUA/* || !luaRunning*/)
		return;

	// First, check if we're being called by anybody
	lua_getfield(LUA, LUA_REGISTRYINDEX, guiCallbackTable);
	
	if (lua_isfunction(LUA, -1)) {
		int ret;

		// We call it now
		numTries = 1000;
		ret = lua_pcall(LUA, 0, 0, 0);
		if (ret != 0) {
#ifdef WIN32
			MessageBox(gApp.hWnd, lua_tostring(LUA, -1), "Lua Error in GUI function", MB_OK);
#else
			fprintf(stderr, "Lua error in gui.register function: %s\n", lua_tostring(LUA, -1));
#endif
			// This is grounds for trashing the function
			lua_pushnil(LUA);
			lua_setfield(LUA, LUA_REGISTRYINDEX, guiCallbackTable);
		}
	}

	// And wreak the stack
	lua_settop(LUA, 0);

	if (gui_used == GUI_CLEAR || !gui_enabled)
		return;

	gui_used = GUI_USED_SINCE_LAST_FRAME;

	for (y = 0; y < LUA_SCREEN_HEIGHT; y++) {
		for (x=0; x < LUA_SCREEN_WIDTH; x++) {
			gui_alpha = gui_data[(y*LUA_SCREEN_WIDTH+x)*4+3];
			if (gui_alpha != 0) {
				if (gui_alpha == 255) {
					XBuf[(y*LUA_SCREEN_WIDTH+x)*4+2]=gui_data[(y*LUA_SCREEN_WIDTH+x)*4+2];
					XBuf[(y*LUA_SCREEN_WIDTH+x)*4+1]=gui_data[(y*LUA_SCREEN_WIDTH+x)*4+1];
					XBuf[(y*LUA_SCREEN_WIDTH+x)*4]=gui_data[(y*LUA_SCREEN_WIDTH+x)*4];
				}
				else {
					gui_red   = gui_data[(y*LUA_SCREEN_WIDTH+x)*4+2];
					gui_green = gui_data[(y*LUA_SCREEN_WIDTH+x)*4+1];
					gui_blue  = gui_data[(y*LUA_SCREEN_WIDTH+x)*4];
					r = XBuf[(y*LUA_SCREEN_WIDTH+x)*4+2];
					g = XBuf[(y*LUA_SCREEN_WIDTH+x)*4+1];
					b = XBuf[(y*LUA_SCREEN_WIDTH+x)*4];
					XBuf[(y*LUA_SCREEN_WIDTH+x)*4+2] = ((gui_red   - r) * gui_alpha / 255 + r) & 255;
					XBuf[(y*LUA_SCREEN_WIDTH+x)*4+1] = ((gui_green - g) * gui_alpha / 255 + g) & 255;
					XBuf[(y*LUA_SCREEN_WIDTH+x)*4]   = ((gui_blue  - b) * gui_alpha / 255 + b) & 255;
				}
			}
		}
	}

	return;
}


void PCSX_LuaClearGui() {
	gui_used = GUI_CLEAR;
}

void PCSX_LuaEnableGui(uint8 enabled) {
	gui_enabled = enabled;
}


lua_State* PCSX_GetLuaState() {
	return LUA;
}
char* PCSX_GetLuaScriptName() {
	return luaScriptName;
}
