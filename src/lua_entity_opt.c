/*
 * Entity metamethod optimization for NecroDancer's ECS.
 *
 * Replaces the Lua __index/__newindex metamethods on component wrapper
 * tables with C implementations.  The Lua versions are interpreted (never
 * JIT-compiled) because sol2 invokes them through C API calls, which the
 * JIT can't trace through.
 *
 * Detection: hook lua_pcall in the GOT.  After each call, probe for the
 * ECS module via ScriptLoader's loadedScripts upvalue (extracted with
 * debug.getupvalue).  The game's strict-mode _G is bypassed with rawget/
 * rawset.  Once entity types are registered, spawn a temporary entity to
 * capture the component metatable, extract defaultValue/invalidWriteError
 * via debug.getupvalue, and replace __index/__newindex with C closures.
 *
 * No LuaJIT headers — all API calls go through dlsym.
 */

#include "lua_entity_opt.h"
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

/* Lua type constants */
#define LUA_TNIL		0
#define LUA_TTABLE		5
#define LUA_TFUNCTION		6
#define LUA_GLOBALSINDEX	(-10002)
#define lua_upvalueindex(i)	(LUA_GLOBALSINDEX - (i))

/* ---- Cached Lua API function pointers ---- */

static int    (*api_rawget)(void *L, int idx);
static void   (*api_pushvalue)(void *L, int idx);
static int    (*api_type)(void *L, int idx);
static void   (*api_settop)(void *L, int idx);
static void   (*api_pushstring)(void *L, const char *s);
static void   (*api_call)(void *L, int nargs, int nresults);
static void   (*api_pushcclosure)(void *L, void *fn, int n);
static int    (*api_pcall)(void *L, int nargs, int nresults, int errfunc);
static int    (*api_loadstring)(void *L, const char *s);
static const char *(*api_tolstring)(void *L, int idx, size_t *len);
static void   (*api_rawset)(void *L, int idx);

static int resolve_lua_api(void)
{
	api_rawget       = dlsym(RTLD_DEFAULT, "lua_rawget");
	api_pushvalue    = dlsym(RTLD_DEFAULT, "lua_pushvalue");
	api_type         = dlsym(RTLD_DEFAULT, "lua_type");
	api_settop       = dlsym(RTLD_DEFAULT, "lua_settop");
	api_pushstring   = dlsym(RTLD_DEFAULT, "lua_pushstring");
	api_call         = dlsym(RTLD_DEFAULT, "lua_call");
	api_pushcclosure = dlsym(RTLD_DEFAULT, "lua_pushcclosure");
	api_pcall        = dlsym(RTLD_DEFAULT, "lua_pcall");
	api_loadstring   = dlsym(RTLD_DEFAULT, "luaL_loadstring");
	api_tolstring    = dlsym(RTLD_DEFAULT, "lua_tolstring");
	api_rawset       = dlsym(RTLD_DEFAULT, "lua_rawset");

	return api_rawget && api_pushvalue && api_type && api_settop &&
	       api_pushstring && api_call && api_pushcclosure &&
	       api_pcall && api_loadstring && api_tolstring && api_rawset;
}

/*
 * component_index(L) — C replacement for componentMetatable.__index
 *
 * Stack on entry: [1]=self, [2]=key
 * Upvalue 1: the original defaultValue function
 *
 * Logic:
 *   id = rawget(self, "_ids")[key]
 *   if id ~= nil then return rawget(self, "_fields")[id]
 *   else return defaultValue(self, key) end
 */
static int component_index(void *L)
{
	api_pushstring(L, "_ids");
	api_rawget(L, 1);          /* [3] = self._ids */

	api_pushvalue(L, 2);
	api_rawget(L, 3);          /* [4] = _ids[key] or nil */

	if (api_type(L, -1) != LUA_TNIL) {
		api_pushstring(L, "_fields");
		api_rawget(L, 1);      /* [5] = self._fields */
		api_pushvalue(L, 4);
		api_rawget(L, -2);     /* [6] = _fields[id] */
		return 1;
	}

	/* Fallback: defaultValue(self, key) */
	api_settop(L, 2);
	api_pushvalue(L, lua_upvalueindex(1));
	api_pushvalue(L, 1);
	api_pushvalue(L, 2);
	api_call(L, 2, 1);
	return 1;
}

/*
 * component_newindex(L) — C replacement for componentMetatable.__newindex
 *
 * Stack on entry: [1]=self, [2]=key, [3]=value
 * Upvalue 1: the original invalidWriteError function
 *
 * Logic:
 *   setter = rawget(self, "_setters")[key]
 *   if setter ~= nil then setter(rawget(self, "_fields"), value)
 *   else invalidWriteError(self, key) end
 */
static int component_newindex(void *L)
{
	api_pushstring(L, "_setters");
	api_rawget(L, 1);          /* [4] = self._setters */

	api_pushvalue(L, 2);
	api_rawget(L, 4);          /* [5] = _setters[key] or nil */

	if (api_type(L, -1) != LUA_TNIL) {
		api_pushstring(L, "_fields");
		api_rawget(L, 1);      /* [6] = self._fields */
		api_pushvalue(L, 3);   /* [7] = value */
		api_call(L, 2, 0);     /* setter(_fields, value) */
		return 0;
	}

	/* Fallback: invalidWriteError(self, key) */
	api_settop(L, 3);
	api_pushvalue(L, lua_upvalueindex(1));
	api_pushvalue(L, 1);
	api_pushvalue(L, 2);
	api_call(L, 2, 0);
	return 0;
}

/* Closure factories — called from Lua to create C closures with upvalues */
static int make_index_closure(void *L)
{
	api_pushcclosure(L, (void *)component_index, 1);
	return 1;
}

static int make_newindex_closure(void *L)
{
	api_pushcclosure(L, (void *)component_newindex, 1);
	return 1;
}

/* ---- Installation ---- */

static int do_lua_string(void *L, const char *s)
{
	int err = api_loadstring(L, s);
	if (err == 0)
		err = api_pcall(L, 0, 0, 0);
	if (err != 0) {
		const char *msg = api_tolstring(L, -1, NULL);
		fprintf(stderr, "resolver: entity_opt: %s\n",
			msg ? msg : "(unknown error)");
		api_settop(L, -2);
	}
	return err;
}

/*
 * Shared Lua snippet to find the ECS module.  Used by both the probe
 * and the install phase.  Accesses ScriptLoader's loadedScripts upvalue
 * via debug.getupvalue to bypass the sandboxed script environments.
 */
static const char find_ecs_lua[] =
	"local sl = package.loaded['core.ScriptLoader']\n"
	"if type(sl) ~= 'table' then error('no ScriptLoader') end\n"
	"local loadedScripts\n"
	"for _, fname in ipairs({'loadScript', 'getGlobal', 'defineGlobal'}) do\n"
	"    local fn = rawget(sl, fname)\n"
	"    if fn then\n"
	"        for i = 1, 30 do\n"
	"            local name, val = debug.getupvalue(fn, i)\n"
	"            if not name then break end\n"
	"            if name == 'loadedScripts' then loadedScripts = val; break end\n"
	"        end\n"
	"        if loadedScripts then break end\n"
	"    end\n"
	"end\n"
	"if not loadedScripts then error('no loadedScripts') end\n"
	"local entry = loadedScripts['system.game.Entities']\n"
	"if not entry then error('Entities not loaded') end\n"
	"local ECS = entry.exports\n"
	"if type(ECS) ~= 'table' then error('bad exports') end\n";

int try_install_entity_opt(void *L)
{
	static int api_resolved = 0;
	if (!api_resolved) {
		if (!resolve_lua_api())
			return 0;
		api_resolved = 1;
	}

	/* Probe: find ECS and check entity types are registered */
	char probe[sizeof(find_ecs_lua) + 128];
	snprintf(probe, sizeof(probe), "%s%s", find_ecs_lua,
		"local t = ECS.getEntityTypesWithComponents({'position'})\n"
		"if not t or #t == 0 then error('no entity types') end\n");

	int err = do_lua_string(L, probe);
	if (err != 0) return 0;

	fprintf(stderr, "resolver: entity_opt: ECS found, installing C metamethods\n");

	/* Register closure factories via rawset to bypass _G strict mode */
	api_pushstring(L, "_machismo_make_index_closure");
	api_pushcclosure(L, (void *)make_index_closure, 0);
	api_rawset(L, LUA_GLOBALSINDEX);

	api_pushstring(L, "_machismo_make_newindex_closure");
	api_pushcclosure(L, (void *)make_newindex_closure, 0);
	api_rawset(L, LUA_GLOBALSINDEX);

	/* Install: find ECS, spawn entity, capture metatable, replace metamethods */
	char install[sizeof(find_ecs_lua) + 2048];
	snprintf(install, sizeof(install), "%s%s", find_ecs_lua,
		"local types = ECS.getEntityTypesWithComponents({'position'})\n"
		"if not types or #types == 0 then\n"
		"    types = ECS.getEntityTypesWithComponents({'health'})\n"
		"end\n"
		"if not types or #types == 0 then\n"
		"    error('entity_opt: no types with position or health')\n"
		"end\n"
		"\n"
		"local entity = ECS.spawn(types[1])\n"
		"\n"
		"local comp\n"
		"for _, cn in ipairs({'position', 'health', 'velocity', 'damage',\n"
		"                     'controllable', 'beatDelay', 'renderable'}) do\n"
		"    local c = entity[cn]\n"
		"    if c and type(c) == 'table' and getmetatable(c) then\n"
		"        comp = c; break\n"
		"    end\n"
		"end\n"
		"if not comp then\n"
		"    ECS.despawn(entity.id)\n"
		"    error('entity_opt: no mutable component')\n"
		"end\n"
		"\n"
		"local cmt = getmetatable(comp)\n"
		"\n"
		"local defaultValue\n"
		"for i = 1, 10 do\n"
		"    local n, v = debug.getupvalue(cmt.__index, i)\n"
		"    if not n then break end\n"
		"    if n == 'defaultValue' then defaultValue = v; break end\n"
		"end\n"
		"\n"
		"local invalidWriteError\n"
		"for i = 1, 10 do\n"
		"    local n, v = debug.getupvalue(cmt.__newindex, i)\n"
		"    if not n then break end\n"
		"    if n == 'invalidWriteError' then invalidWriteError = v; break end\n"
		"end\n"
		"\n"
		"if not defaultValue then\n"
		"    ECS.despawn(entity.id)\n"
		"    error('entity_opt: no defaultValue upvalue')\n"
		"end\n"
		"if not invalidWriteError then\n"
		"    ECS.despawn(entity.id)\n"
		"    error('entity_opt: no invalidWriteError upvalue')\n"
		"end\n"
		"\n"
		"local mkIdx = rawget(_G, '_machismo_make_index_closure')\n"
		"local mkNew = rawget(_G, '_machismo_make_newindex_closure')\n"
		"cmt.__index = mkIdx(defaultValue)\n"
		"cmt.__newindex = mkNew(invalidWriteError)\n"
		"\n"
		"ECS.despawn(entity.id)\n"
		"rawset(_G, '_machismo_make_index_closure', nil)\n"
		"rawset(_G, '_machismo_make_newindex_closure', nil)\n"
	);

	err = do_lua_string(L, install);
	if (err != 0)
		return 0;

	fprintf(stderr, "resolver: entity metamethod optimizations installed\n");
	return 1;
}
