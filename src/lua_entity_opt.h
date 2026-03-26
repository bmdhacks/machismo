#ifndef LUA_ENTITY_OPT_H
#define LUA_ENTITY_OPT_H

/*
 * Entity metamethod optimization — replaces Lua __index/__newindex on
 * component wrapper tables with C implementations.
 *
 * Returns 1 if installed, 0 if entity system not ready yet.
 * Safe to call repeatedly; becomes a no-op after successful install.
 */
int try_install_entity_opt(void *L);

#endif /* LUA_ENTITY_OPT_H */
