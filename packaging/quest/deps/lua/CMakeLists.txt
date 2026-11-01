# Static Lua for the Quest build (Lua ships no CMake). LUA_SRC = <lua>/src.
cmake_minimum_required(VERSION 3.18)
project(lua C)
set(LUA_SRC "" CACHE PATH "Lua src/ directory")
file(GLOB LUA_C "${LUA_SRC}/*.c")
list(REMOVE_ITEM LUA_C "${LUA_SRC}/lua.c" "${LUA_SRC}/luac.c" "${LUA_SRC}/onelua.c")
add_library(lua STATIC ${LUA_C})
target_compile_definitions(lua PRIVATE LUA_USE_POSIX)
target_include_directories(lua PUBLIC ${LUA_SRC})
install(TARGETS lua ARCHIVE DESTINATION lib)
install(FILES ${LUA_SRC}/lua.h ${LUA_SRC}/luaconf.h ${LUA_SRC}/lualib.h ${LUA_SRC}/lauxlib.h ${LUA_SRC}/lua.hpp
        DESTINATION include)
