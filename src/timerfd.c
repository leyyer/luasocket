/*=========================================================================*\
 * Timerfd
 * LuaSocket toolkit
 \*=========================================================================*/
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "auxiliar.h"
#include "socket.h"
#include "options.h"
#include "unix.h"
#include <sys/un.h>
#include <sys/timerfd.h>

#define TFD_CLASS_NAME  "timerfd{client}"
#define TFD_GEN_NAME    "timerfd{any}"

struct tmfd {
	int sock;
	uint64_t start;
};

/*
 * a timeout object, wrap linux timerfd.
 */

/*=========================================================================*\
 * Internal function prototypes
 \*=========================================================================*/
static int global_create(lua_State *L);
static int meth_clear(lua_State *L);
static int meth_close(lua_State *L);
static int meth_settimeout(lua_State *L);
static int meth_gettime(lua_State *L);
static int meth_getfd(lua_State *L);
static int meth_getstart(lua_State *L);

/* timerfd object methods */
static luaL_Reg timerfd_methods[] = {
	{"__gc",        meth_close},
	{"__tostring",  auxiliar_tostring},
	{"close",       meth_close},
	{"getfd",       meth_getfd},
	{"clear",       meth_clear},
	{"timeout",     meth_settimeout},
	{"getstart",    meth_getstart},
	{"elapse",      meth_gettime},
	{NULL,          NULL}
};

/*-------------------------------------------------------------------------*\
 * Initializes module
 \*-------------------------------------------------------------------------*/
LUASOCKET_API int luaopen_socket_timerfd(lua_State *L) {
	/* create classes */
	auxiliar_newclass(L, TFD_CLASS_NAME, timerfd_methods);
	/* create class groups */
	auxiliar_add2group(L, TFD_CLASS_NAME, TFD_GEN_NAME);
	lua_pushcfunction(L, global_create);
	return 1;
}

/*=========================================================================*\
 * Lua methods
 \*=========================================================================*/
/*-------------------------------------------------------------------------*\
 * Just call buffered IO methods
 \*-------------------------------------------------------------------------*/
static int meth_clear(lua_State *L) {
	struct tmfd *un = (struct tmfd *) auxiliar_checkclass(L, TFD_CLASS_NAME, 1);
	uint64_t v = 0;
	int len;

	if (un->sock == SOCKET_INVALID) {
		return  0;
	}

	do {
		len = read(un->sock, &v, sizeof v);
	} while (len == -1 && errno == EINTR);
	
	lua_pushboolean(L, len == sizeof v);
	return 1;
}

static uint64_t __get_time(void)
{
	struct timespec now;
	uint64_t ms;

	clock_gettime(CLOCK_MONOTONIC, &now);
	ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;

	return ms;
}

static void tfd_set_timeout(int fd, double sec, double eps)
{
	struct timespec now;
	struct itimerspec new_value;
	uint64_t ms; 

	ms = sec * 1000;
	memset(&new_value, 0, sizeof new_value);
	clock_gettime(CLOCK_MONOTONIC, &now);
	new_value.it_value.tv_sec = now.tv_sec + (ms / 1000);
	new_value.it_value.tv_nsec = now.tv_nsec + (ms % 1000) * 1000000;

	while (new_value.it_value.tv_nsec >= 1000000000) {
		new_value.it_value.tv_nsec -= 1000000000;
		new_value.it_value.tv_sec += 1;
	}

	ms = eps * 1000;
	if (ms > 0) {
		new_value.it_interval.tv_sec = ms / 1000;
		new_value.it_interval.tv_nsec = (ms % 1000) * 1000000;
		while (new_value.it_interval.tv_nsec >= 1000000000) {
			new_value.it_interval.tv_nsec -= 1000000000;
			new_value.it_value.tv_sec += 1;
		}
	}
	timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL);
}

/*-------------------------------------------------------------------------*\
 * Select support methods
 \*-------------------------------------------------------------------------*/
static int meth_getfd(lua_State *L) {
	struct tmfd * un = (struct tmfd *) auxiliar_checkgroup(L, TFD_GEN_NAME, 1);
	lua_pushinteger(L, un->sock);
	return 1;
}

static int meth_getstart(lua_State *L)
{
	double s;
	struct tmfd * un = (struct tmfd *) auxiliar_checkgroup(L, TFD_GEN_NAME, 1);

	s = un->start / 1000.0;
	lua_pushnumber(L, s);

	return 1;
}

/*-------------------------------------------------------------------------*\
 * Closes socket used by object
 \*-------------------------------------------------------------------------*/
static int meth_close(lua_State *L)
{
	struct tmfd * un = (struct tmfd *) auxiliar_checkgroup(L, TFD_GEN_NAME, 1);
	socket_destroy(&un->sock);
	lua_pushnumber(L, 1);
	return 1;
}


/*-------------------------------------------------------------------------*\
 * Just call tm methods
 \*-------------------------------------------------------------------------*/
static int meth_settimeout(lua_State *L) {
	struct tmfd *un = (struct tmfd *) auxiliar_checkgroup(L, TFD_GEN_NAME, 1);
	double eps = 0.0, itv = 0.0;

	if (un->sock == SOCKET_INVALID)
		return 0;

	eps = luaL_checknumber(L, 2);
	if (lua_gettop(L) >= 3) {
		itv = luaL_checknumber(L, 3);
	} 
	tfd_set_timeout(un->sock, eps, itv);
	un->start = __get_time();
	return 0;
}

static int meth_gettime(lua_State *L)
{
	uint64_t now;
	double diff;
	struct tmfd *un = (struct tmfd *) auxiliar_checkgroup(L, TFD_GEN_NAME, 1);

	now = __get_time();
	diff = (now - un->start) / 1000.0;
	lua_pushnumber(L, diff);
	return 1;
}

/*-------------------------------------------------------------------------*\
 * Creates a timerfd object
 \*-------------------------------------------------------------------------*/
static int global_create(lua_State *L) {
	double eps, itv = 0.0;

	eps = luaL_checknumber(L, 1);
	if (lua_gettop(L) >= 2)
		itv = luaL_checknumber(L, 2);

	/* allocate unix object */
	struct tmfd *un = (struct tmfd *) lua_newuserdata(L, sizeof(*un));

	/* open timerfd device */
	t_socket sock = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

	/* printf("open %s on %d\n", path, sock); */

	if (sock < 0)  {
		lua_pushnil(L);
		lua_pushstring(L, socket_strerror(errno));
		lua_pushnumber(L, errno);
		return 3;
	}
	/* set its type as client object */
	auxiliar_setclass(L, TFD_CLASS_NAME, -1);
	/* initialize remaining structure fields */
	un->sock = sock;
	if (eps > 0.0) {
		tfd_set_timeout(sock, eps, itv);
	}
	un->start = __get_time();
	lua_pushnumber(L, (double)(un->start / 1000.0));

	return 2;
}

