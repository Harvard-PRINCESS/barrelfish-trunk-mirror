/**
 * \file
 * \brief Test the LUA interpreter library
 */
/*
 * Copyright (c) 2013, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <barrelfish/barrelfish.h>

#include <lua/lua.h>
#include <lua/lauxlib.h>
#include <lua/lualib.h>

static char* program = "-- sieve.lua\n"
"-- the sieve of Eratosthenes programmed with coroutines\n"
"-- typical usage: lua -e N=500 sieve.lua | column\n"
"\n"
"-- generate all the numbers from 2 to n\n"
"function gen (n)\n"
"  return coroutine.wrap(function ()\n"
"    for i=2,n do coroutine.yield(i) end\n"
"  end)\n"
"end\n"
"\n"
"-- filter the numbers generated by `g', removing multiples of `p'\n"
"function filter (p, g)\n"
"  return coroutine.wrap(function ()\n"
"    for n in g do\n"
"      if n%p ~= 0 then coroutine.yield(n) end\n"
"    end\n"
"  end)\n"
"end\n"
"\n"
"N=N or 500      -- from command line\n"
"x = gen(N)      -- generate primes up to N\n"
"while 1 do\n"
"  local n = x()     -- pick a number until done\n"
"  if n == nil then break end\n"
"  print(n)      -- must be a prime number\n"
"  x = filter(n, x)  -- now remove its multiples\n"
"end\n";





int main (void){
    int error;
    lua_State *L = luaL_newstate();   /* opens Lua */
    luaL_openlibs(L);

    error = luaL_loadbuffer(L, program, strlen(program), "line") || lua_pcall(L, 0, 0, 0);
    if (error) {
        fprintf(stderr, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);  /* pop error message from the stack */
    }

    lua_close(L);
    return 0;
}
