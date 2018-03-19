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

/*
Reuses userdata definition from unix.h, since it is useful for all
stream-like objects.

If we stored the timerfd path for use in error messages or userdata
printing, we might need our own userdata definition.

Group usage is semi-inherited from unix.c, but unnecessary since we
have only one object type.
*/

/*=========================================================================*\
* Internal function prototypes
\*=========================================================================*/
static int global_create(lua_State *L);
static int meth_receive(lua_State *L);
static int meth_close(lua_State *L);
static int meth_settimeout(lua_State *L);
static int meth_getfd(lua_State *L);

/* timerfd object methods */
static luaL_Reg timerfd_methods[] = {
    {"__gc",        meth_close},
    {"__tostring",  auxiliar_tostring},
    {"close",       meth_close},
    {"getfd",       meth_getfd},
    {"receive",     meth_receive},
    {"settimeout",  meth_settimeout},
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
static int meth_receive(lua_State *L) {
    p_unix un = (p_unix) auxiliar_checkclass(L, TFD_CLASS_NAME, 1);
    return buffer_meth_receive(L, &un->buf);
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
    p_unix un = (p_unix) auxiliar_checkgroup(L, TFD_GEN_NAME, 1);
    lua_pushnumber(L, (int) un->sock);
    return 1;
}

/*-------------------------------------------------------------------------*\
* Closes socket used by object
\*-------------------------------------------------------------------------*/
static int meth_close(lua_State *L)
{
    p_unix un = (p_unix) auxiliar_checkgroup(L, TFD_GEN_NAME, 1);
    socket_destroy(&un->sock);
    lua_pushnumber(L, 1);
    return 1;
}


/*-------------------------------------------------------------------------*\
* Just call tm methods
\*-------------------------------------------------------------------------*/
static int meth_settimeout(lua_State *L) {
    p_unix un = (p_unix) auxiliar_checkgroup(L, TFD_GEN_NAME, 1);
	double eps = 0.0, itv = 0.0;

	if (un->sock == SOCKET_INVALID)
		return 0;

	eps = luaL_checknumber(L, 2);
	if (lua_gettop(L) >= 3) {
		itv = luaL_checknumber(L, 3);
	} else {
		itv = eps;
	}

	tfd_set_timeout(un->sock, eps, itv);
	return 0;
}

/*=========================================================================*\
* Library functions
\*=========================================================================*/

static int tfd_read(p_socket ps, char *data, size_t count, size_t *got, p_timeout tm)
{
	uint64_t v;
	int len;

	(void)tm;
    if (*ps == SOCKET_INVALID) return IO_CLOSED;
	do {
		if (count >= sizeof v) {
			len = read(*ps, data, sizeof v);
		} else {
			len = read(*ps, &v, sizeof v);
			if (len > 0) {
				memcpy(data, &v, count);
			}
		}
	} while ((len == -1) && errno == EINTR);

	if (len < 0) {
		return errno;
	}
	*got = len;
	return IO_DONE;
}

/*-------------------------------------------------------------------------*\
* Creates a timerfd object
\*-------------------------------------------------------------------------*/
static int global_create(lua_State *L) {
	double eps, itv = 0.0;

	eps = luaL_checknumber(L, 1);
	if (lua_gettop(L) >= 2)
		itv = luaL_checknumber(L, 2);
	else
		itv = eps;

    /* allocate unix object */
    p_unix un = (p_unix) lua_newuserdata(L, sizeof(t_unix));

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
    io_init(&un->io, (p_send) NULL, (p_recv) tfd_read,
            (p_error) socket_ioerror, &un->sock);
    timeout_init(&un->tm, -1, -1);
    buffer_init(&un->buf, &un->io, &un->tm);
	if (eps > 0.0) {
		tfd_set_timeout(sock, eps, itv);
	}
    return 1;
}

