#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include "ngx_http_lua_socket_udp.h"
#include "ngx_http_lua_socket_tcp.h"
#include "ngx_http_lua_util.h"
#include "ngx_http_lua_contentby.h"
#include "ngx_http_lua_output.h"


#define UDP_MAX_DATAGRAM_SIZE 8192


ngx_int_t ngx_udp_connect(ngx_udp_connection_t *uc);


static int ngx_http_lua_socket_udp(lua_State *L);
static int ngx_http_lua_socket_udp_setpeername(lua_State *L);
static int ngx_http_lua_socket_udp_send(lua_State *L);
static int ngx_http_lua_socket_udp_receive(lua_State *L);
static int ngx_http_lua_socket_udp_settimeout(lua_State *L);
static void ngx_http_lua_socket_udp_finalize(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u);
static int ngx_http_lua_socket_udp_upstream_destroy(lua_State *L);
static int ngx_http_lua_socket_resolve_retval_handler(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u, lua_State *L);
static void ngx_http_lua_socket_resolve_handler(ngx_resolver_ctx_t *ctx);
static int ngx_http_lua_socket_error_retval_handler(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u, lua_State *L);
static void ngx_http_lua_socket_udp_handle_error(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u, ngx_uint_t ft_type);
static void ngx_http_lua_socket_udp_cleanup(void *data);
static void ngx_http_lua_socket_udp_handler(ngx_event_t *ev);
static void ngx_http_lua_socket_dummy_handler(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u);
static int ngx_http_lua_socket_udp_receive_retval_handler(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u, lua_State *L);
static ngx_int_t ngx_http_lua_socket_udp_read(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u);
static void ngx_http_lua_socket_udp_read_handler(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u);
static void ngx_http_lua_socket_udp_handle_success(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u);


enum {
    SOCKET_CTX_INDEX = 1,
    SOCKET_TIMEOUT_INDEX = 2
};


static char ngx_http_lua_socket_udp_metatable_key;
static u_char ngx_http_lua_socket_udp_buffer[UDP_MAX_DATAGRAM_SIZE];


void
ngx_http_lua_inject_socket_udp_api(ngx_log_t *log, lua_State *L)
{
    lua_getfield(L, -1, "socket"); /* ngx socket */

    lua_pushcfunction(L, ngx_http_lua_socket_udp);
    lua_setfield(L, -2, "udp"); /* ngx socket */

    /* udp socket object metatable */
    lua_pushlightuserdata(L, &ngx_http_lua_socket_udp_metatable_key);
    lua_createtable(L, 0 /* narr */, 4 /* nrec */);

    lua_pushcfunction(L, ngx_http_lua_socket_udp_setpeername);
    lua_setfield(L, -2, "setpeername"); /* ngx socket mt */

    lua_pushcfunction(L, ngx_http_lua_socket_udp_send);
    lua_setfield(L, -2, "send");

    lua_pushcfunction(L, ngx_http_lua_socket_udp_receive);
    lua_setfield(L, -2, "receive");

    lua_pushcfunction(L, ngx_http_lua_socket_udp_settimeout);
    lua_setfield(L, -2, "settimeout"); /* ngx socket mt */

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_rawset(L, LUA_REGISTRYINDEX);
    /* }}} */

    lua_pop(L, 1);
}


static int
ngx_http_lua_socket_udp(lua_State *L)
{
    ngx_http_request_t      *r;
    ngx_http_lua_ctx_t      *ctx;

    if (lua_gettop(L) != 0) {
        return luaL_error(L, "expecting zero arguments, but got %d",
                          lua_gettop(L));
    }

    lua_pushlightuserdata(L, &ngx_http_lua_request_key);
    lua_rawget(L, LUA_GLOBALSINDEX);
    r = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (r == NULL) {
        return luaL_error(L, "no request found");
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        return luaL_error(L, "no ctx found");
    }

    ngx_http_lua_check_context(L, ctx, NGX_HTTP_LUA_CONTEXT_REWRITE
                               | NGX_HTTP_LUA_CONTEXT_ACCESS
                               | NGX_HTTP_LUA_CONTEXT_CONTENT);

    lua_createtable(L, 3 /* narr */, 1 /* nrec */);
    lua_pushlightuserdata(L, &ngx_http_lua_socket_udp_metatable_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);

    dd("top: %d", lua_gettop(L));

    return 1;
}


static int
ngx_http_lua_socket_udp_setpeername(lua_State *L)
{
    ngx_http_request_t          *r;
    ngx_http_lua_ctx_t          *ctx;
    ngx_str_t                    host;
    int                          port;
    ngx_resolver_ctx_t          *rctx, temp;
    ngx_http_core_loc_conf_t    *clcf;
    int                          saved_top;
    int                          n;
    u_char                      *p;
    size_t                       len;
    ngx_url_t                    url;
    ngx_int_t                    rc;
    ngx_http_lua_loc_conf_t     *llcf;
    ngx_udp_connection_t        *uc;
    int                          timeout;

    ngx_http_lua_socket_udp_upstream_t      *u;

    n = lua_gettop(L);
    if (n != 2 && n != 3) {
        return luaL_error(L, "ngx.socket.udp setpeername: expecting 2 or 3 "
                          "arguments (including the object), but seen %d", n);
    }

    lua_pushlightuserdata(L, &ngx_http_lua_request_key);
    lua_rawget(L, LUA_GLOBALSINDEX);
    r = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (r == NULL) {
        return luaL_error(L, "no request found");
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        return luaL_error(L, "no ctx found");
    }

    ngx_http_lua_check_context(L, ctx, NGX_HTTP_LUA_CONTEXT_REWRITE
                               | NGX_HTTP_LUA_CONTEXT_ACCESS
                               | NGX_HTTP_LUA_CONTEXT_CONTENT);

    luaL_checktype(L, 1, LUA_TTABLE);

    p = (u_char *) luaL_checklstring(L, 2, &len);

    host.data = ngx_palloc(r->pool, len + 1);
    if (host.data == NULL) {
        return luaL_error(L, "out of memory");
    }

    host.len = len;

    ngx_memcpy(host.data, p, len);
    host.data[len] = '\0';

    if (n == 3) {
        port = luaL_checkinteger(L, 3);

        if (port < 0 || port > 65536) {
            lua_pushnil(L);
            lua_pushfstring(L, "bad port number: %d", port);
            return 2;
        }

    } else { /* n == 2 */
        port = 0;
    }

    lua_rawgeti(L, 1, SOCKET_CTX_INDEX);
    u = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (u) {
        if (u->udp_connection.connection) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "lua udp socket reconnect without shutting down");

            ngx_http_lua_socket_udp_finalize(r, u);
        }

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "lua reuse socket upstream ctx");

    } else {
        u = lua_newuserdata(L, sizeof(ngx_http_lua_socket_udp_upstream_t));
        if (u == NULL) {
            return luaL_error(L, "out of memory");
        }

#if 1
        lua_createtable(L, 0 /* narr */, 1 /* nrec */); /* metatable */
        lua_pushcfunction(L, ngx_http_lua_socket_udp_upstream_destroy);
        lua_setfield(L, -2, "__gc");
        lua_setmetatable(L, -2);
#endif

        lua_rawseti(L, 1, SOCKET_CTX_INDEX);
    }

    ngx_memzero(u, sizeof(ngx_http_lua_socket_udp_upstream_t));

    u->request = r; /* set the controlling request */
    llcf = ngx_http_get_module_loc_conf(r, ngx_http_lua_module);

    u->conf = llcf;

    uc = &u->udp_connection;

    uc->log = *r->connection->log;

    dd("lua peer connection log: %p", &uc->log);

    lua_rawgeti(L, 1, SOCKET_TIMEOUT_INDEX);
    timeout = (ngx_int_t) lua_tointeger(L, -1);
    lua_pop(L, 1);

    if (timeout > 0) {
        u->read_timeout = (ngx_msec_t) timeout;

    } else {
        u->read_timeout = u->conf->read_timeout;
    }

    ngx_memzero(&url, sizeof(ngx_url_t));

    url.url.len = host.len;
    url.url.data = host.data;
    url.default_port = port;
    url.no_resolve = 1;

    if (ngx_parse_url(r->pool, &url) != NGX_OK) {
        lua_pushnil(L);

        if (url.err) {
            lua_pushfstring(L, "failed to parse host name \"%s\": %s",
                            host.data, url.err);

        } else {
            lua_pushfstring(L, "failed to parse host name \"%s\"", host.data);
        }

        return 2;
    }

    u->resolved = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_resolved_t));
    if (u->resolved == NULL) {
        return luaL_error(L, "out of memory");
    }

    if (url.addrs && url.addrs[0].sockaddr) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "lua udp socket network address given directly");

        u->resolved->sockaddr = url.addrs[0].sockaddr;
        u->resolved->socklen = url.addrs[0].socklen;
        u->resolved->naddrs = 1;
        u->resolved->host = url.addrs[0].name;

    } else {
        u->resolved->host = host;
        u->resolved->port = (in_port_t) port;
    }

    if (u->resolved->sockaddr) {
        rc = ngx_http_lua_socket_resolve_retval_handler(r, u, L);
        if (rc == NGX_AGAIN) {
            return lua_yield(L, 0);
        }

        return rc;
    }

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    temp.name = host;
    rctx = ngx_resolve_start(clcf->resolver, &temp);
    if (rctx == NULL) {
        u->ft_type |= NGX_HTTP_LUA_SOCKET_FT_RESOLVER;
        lua_pushnil(L);
        lua_pushliteral(L, "failed to start the resolver");
        return 2;
    }

    if (rctx == NGX_NO_RESOLVER) {
        u->ft_type |= NGX_HTTP_LUA_SOCKET_FT_RESOLVER;
        lua_pushnil(L);
        lua_pushfstring(L, "no resolver defined to resolve \"%s\"", host.data);
        return 2;
    }

    rctx->name = host;
    rctx->type = NGX_RESOLVE_A;
    rctx->handler = ngx_http_lua_socket_resolve_handler;
    rctx->data = u;
    rctx->timeout = clcf->resolver_timeout;

    u->resolved->ctx = rctx;

    saved_top = lua_gettop(L);

    if (ngx_resolve_name(rctx) != NGX_OK) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "lua udp socket fail to run resolver immediately");

        u->ft_type |= NGX_HTTP_LUA_SOCKET_FT_RESOLVER;

        u->resolved->ctx = NULL;
        lua_pushnil(L);
        lua_pushfstring(L, "%s could not be resolved", host.data);

        return 2;
    }

    if (u->waiting == 1) {
        /* resolved and already connecting */
        return lua_yield(L, 0);
    }

    n = lua_gettop(L) - saved_top;
    if (n) {
        /* errors occurred during resolving or connecting
         * or already connected */
        return n;
    }

    /* still resolving */

    u->waiting = 1;
    u->prepare_retvals = ngx_http_lua_socket_resolve_retval_handler;

    ctx->data = u;
    ctx->udp_socket_busy = 1;
    ctx->udp_socket_ready = 0;

    if (ctx->entered_content_phase) {
        r->write_event_handler = ngx_http_lua_content_wev_handler;
    }

    return lua_yield(L, 0);
}


static void
ngx_http_lua_socket_resolve_handler(ngx_resolver_ctx_t *ctx)
{
    ngx_http_request_t                  *r;
    ngx_http_upstream_resolved_t        *ur;
    ngx_http_lua_ctx_t                  *lctx;
    lua_State                           *L;
    ngx_http_lua_socket_udp_upstream_t  *u;
    u_char                              *p;
    size_t                               len;
    struct sockaddr_in                  *sin;
    ngx_uint_t                           i;
    unsigned                             waiting;

    u = ctx->data;
    r = u->request;
    ur = u->resolved;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua udp socket resolve handler");

    lctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);

    L = lctx->cc;

    dd("setting socket_ready to 1");

    waiting = u->waiting;

    if (ctx->state) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "lua udp socket resolver error: %s (waiting: %d)",
                       ngx_resolver_strerror(ctx->state), (int) u->waiting);

        lua_pushnil(L);
        lua_pushlstring(L, (char *) ctx->name.data, ctx->name.len);
        lua_pushfstring(L, " could not be resolved (%d: %s)",
                        (int) ctx->state,
                        ngx_resolver_strerror(ctx->state));
        lua_concat(L, 2);

        u->prepare_retvals = ngx_http_lua_socket_error_retval_handler;
        ngx_http_lua_socket_udp_handle_error(r, u,
                                             NGX_HTTP_LUA_SOCKET_FT_RESOLVER);
        return;
    }

    ur->naddrs = ctx->naddrs;
    ur->addrs = ctx->addrs;

#if (NGX_DEBUG)
    {
    in_addr_t   addr;
    ngx_uint_t  i;

    for (i = 0; i < ctx->naddrs; i++) {
        dd("addr i: %d %p", (int) i,  &ctx->addrs[i]);

        addr = ntohl(ctx->addrs[i]);

        ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "name was resolved to %ud.%ud.%ud.%ud",
                       (addr >> 24) & 0xff, (addr >> 16) & 0xff,
                       (addr >> 8) & 0xff, addr & 0xff);
    }
    }
#endif

    if (ur->naddrs == 0) {
        u->ft_type |= NGX_HTTP_LUA_SOCKET_FT_RESOLVER;

        lua_pushnil(L);
        lua_pushliteral(L, "name cannot be resolved to a address");
        return;
    }

    if (ur->naddrs == 1) {
        i = 0;

    } else {
        i = ngx_random() % ur->naddrs;
    }

    dd("selected addr index: %d", (int) i);

    len = NGX_INET_ADDRSTRLEN + sizeof(":65536") - 1;

    p = ngx_pnalloc(r->pool, len + sizeof(struct sockaddr_in));
    if (p == NULL) {
        u->ft_type |= NGX_HTTP_LUA_SOCKET_FT_RESOLVER;

        lua_pushnil(L);
        lua_pushliteral(L, "out of memory");
        return;
    }

    sin = (struct sockaddr_in *) &p[len];
    ngx_memzero(sin, sizeof(struct sockaddr_in));

    len = ngx_inet_ntop(AF_INET, &ur->addrs[i], p, NGX_INET_ADDRSTRLEN);
    len = ngx_sprintf(&p[len], ":%d", ur->port) - p;

    sin->sin_family = AF_INET;
    sin->sin_port = htons(ur->port);
    sin->sin_addr.s_addr = ur->addrs[i];

    ur->sockaddr = (struct sockaddr *) sin;
    ur->socklen = sizeof(struct sockaddr_in);

    ur->host.data = p;
    ur->host.len = len;
    ur->naddrs = 1;

    ur->ctx = NULL;

    ngx_resolve_name_done(ctx);

    u->waiting = 0;

    if (waiting) {
        lctx->udp_socket_busy = 0;
        lctx->udp_socket_ready = 1;
        r->write_event_handler(r);

    } else {
        (void) ngx_http_lua_socket_resolve_retval_handler(r, u, L);
    }
}


static int
ngx_http_lua_socket_resolve_retval_handler(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u, lua_State *L)
{
    ngx_http_lua_ctx_t              *ctx;
    ngx_udp_connection_t            *uc;
    ngx_connection_t                *c;
    ngx_http_cleanup_t              *cln;
    ngx_http_upstream_resolved_t    *ur;
    ngx_int_t                        rc;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua udp socket resolve retval handler");

    if (u->ft_type & NGX_HTTP_LUA_SOCKET_FT_RESOLVER) {
        return 2;
    }

    uc = &u->udp_connection;

    ur = u->resolved;

    if (ur->sockaddr) {
        uc->sockaddr = ur->sockaddr;
        uc->socklen = ur->socklen;
        uc->server = ur->host;

    } else {
        lua_pushnil(L);
        lua_pushliteral(L, "resolver not working");
        return 2;
    }

    rc = ngx_udp_connect(uc);

    if (u->cleanup == NULL) {
        cln = ngx_http_cleanup_add(r, 0);
        if (cln == NULL) {
            u->ft_type |= NGX_HTTP_LUA_SOCKET_FT_ERROR;
            lua_pushnil(L);
            lua_pushliteral(L, "out of memory");
            return 2;
        }

        cln->handler = ngx_http_lua_socket_udp_cleanup;
        cln->data = u;
        u->cleanup = &cln->handler;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua udp socket connect: %i", rc);

    if (rc != NGX_OK) {
        u->ft_type |= NGX_HTTP_LUA_SOCKET_FT_ERROR;
        lua_pushnil(L);
        lua_pushliteral(L, "connect peer error");
        return 2;
    }

    /* rc == NGX_OK */

    c = uc->connection;

    c->data = u;

    c->write->handler = NULL;
    c->read->handler = ngx_http_lua_socket_udp_handler;
    c->read->resolver = 0;

    c->pool = r->pool;
    c->log = r->connection->log;
    c->read->log = c->log;
    c->write->log = c->log;

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);

    ctx->data = u;

    u->read_event_handler = ngx_http_lua_socket_dummy_handler;

    lua_pushinteger(L, 1);
    return 1;
}


static int
ngx_http_lua_socket_error_retval_handler(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u, lua_State *L)
{
    u_char           errstr[NGX_MAX_ERROR_STR];
    u_char          *p;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua udp socket error retval handler");

    if (u->ft_type & NGX_HTTP_LUA_SOCKET_FT_RESOLVER) {
        return 2;
    }

    lua_pushnil(L);

    if (u->ft_type & NGX_HTTP_LUA_SOCKET_FT_PARTIALWRITE) {
        lua_pushliteral(L, "partial write");

    } else if (u->ft_type & NGX_HTTP_LUA_SOCKET_FT_TIMEOUT) {
        lua_pushliteral(L, "timeout");

    } else if (u->ft_type & NGX_HTTP_LUA_SOCKET_FT_CLOSED) {
        lua_pushliteral(L, "closed");

    } else if (u->ft_type & NGX_HTTP_LUA_SOCKET_FT_BUFTOOSMALL) {
        lua_pushliteral(L, "buffer too small");

    } else if (u->ft_type & NGX_HTTP_LUA_SOCKET_FT_NOMEM) {
        lua_pushliteral(L, "out of memory");

    } else {

        if (u->socket_errno) {
#if (nginx_version >= 1000000)
            p = ngx_strerror(u->socket_errno, errstr, sizeof(errstr));
#else
            p = ngx_strerror_r(u->socket_errno, errstr, sizeof(errstr));
#endif
            /* for compatibility with LuaSocket */
            ngx_strlow(errstr, errstr, p - errstr);
            lua_pushlstring(L, (char *) errstr, p - errstr);

        } else {
            lua_pushliteral(L, "error");
        }
    }

    return 2;
}


static int
ngx_http_lua_socket_udp_send(lua_State *L)
{
    ssize_t                              n;
    ngx_http_request_t                  *r;
    u_char                              *p;
    size_t                               len;
    ngx_http_lua_socket_udp_upstream_t  *u;
    int                                  type;
    const char                          *msg;
    ngx_str_t                            query;
    /* TODO: add support for the optional "i" and "j" arguments */

    if (lua_gettop(L) != 2) {
        return luaL_error(L, "expecting 2 arguments (including the object), "
                          "but got %d", lua_gettop(L));
    }

    lua_pushlightuserdata(L, &ngx_http_lua_request_key);
    lua_rawget(L, LUA_GLOBALSINDEX);
    r = lua_touserdata(L, -1);
    lua_pop(L, 1);

    luaL_checktype(L, 1, LUA_TTABLE);

    lua_rawgeti(L, 1, SOCKET_CTX_INDEX);
    u = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (u == NULL || u->udp_connection.connection == NULL || u->ft_type) {
        ngx_log_error(NGX_LOG_ERR, &u->udp_connection.log, 0,
                      "attempt to send data on a closed socket: u:%p, c:%p, "
                      "ft:%ui",
                      u, u ? u->udp_connection.connection : NULL,
                      u ? u->ft_type : 0);

        lua_pushnil(L);
        lua_pushliteral(L, "closed");
        return 2;
    }

    type = lua_type(L, 2);
    switch (type) {
        case LUA_TNUMBER:
        case LUA_TSTRING:
            lua_tolstring(L, 2, &len);
            break;

        case LUA_TTABLE:
            len = ngx_http_lua_calc_strlen_in_table(L, 2, 2, 1 /* strict */);
            break;

        default:
            msg = lua_pushfstring(L, "string, number, boolean, nil, "
                    "or array table expected, got %s",
                    lua_typename(L, type));

            return luaL_argerror(L, 2, msg);
    }

    query.data = lua_newuserdata(L, len);
    query.len = len;

    switch (type) {
        case LUA_TNUMBER:
        case LUA_TSTRING:
            p = (u_char *) lua_tolstring(L, 2, &len);
            ngx_memcpy(query.data, (u_char *) p, len);
            break;

        case LUA_TTABLE:
            (void) ngx_http_lua_copy_str_in_table(L, 2, query.data);
            break;

        default:
            return luaL_error(L, "impossible to reach here");
    }

    u->ft_type = 0;

    /* mimic ngx_http_upstream_init_request here */

#if 1
    u->waiting = 0;
#endif

    dd("sending query %.*s", (int) query.len, query.data);

    n = ngx_send(u->udp_connection.connection, query.data, query.len);

    dd("ngx_send returns %d (query len %d)", (int) n, (int) query.len);

    if (n == NGX_ERROR || n == NGX_AGAIN) {
        u->socket_errno = ngx_socket_errno;

        return ngx_http_lua_socket_error_retval_handler(r, u, L);
    }

    if (n != (ssize_t) query.len) {
        dd("not the while query was sent");

        u->ft_type |= NGX_HTTP_LUA_SOCKET_FT_PARTIALWRITE;
        return ngx_http_lua_socket_error_retval_handler(r, u, L);
    }

    dd("n == len");

    lua_pushinteger(L, 1);
    return 1;
}


static int
ngx_http_lua_socket_udp_receive(lua_State *L)
{
    ngx_http_request_t                  *r;
    ngx_http_lua_socket_udp_upstream_t  *u;
    ngx_int_t                            rc;
    ngx_http_lua_ctx_t                  *ctx;
    size_t                               size;
    int                                  nargs;

    nargs = lua_gettop(L);
    if (nargs != 1 && nargs != 2) {
        return luaL_error(L, "expecting 1 or 2 arguments "
                          "(including the object), but got %d", nargs);
    }

    lua_pushlightuserdata(L, &ngx_http_lua_request_key);
    lua_rawget(L, LUA_GLOBALSINDEX);
    r = lua_touserdata(L, -1);
    lua_pop(L, 1);

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua udp socket calling receive() method");

    luaL_checktype(L, 1, LUA_TTABLE);

    lua_rawgeti(L, 1, SOCKET_CTX_INDEX);
    u = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (u == NULL || u->udp_connection.connection == NULL || u->ft_type) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "attempt to receive data on a closed socket: u:%p, c:%p, "
                      "ft:%ui eof:%ud",
                      u, u ? u->udp_connection.connection : NULL,
                      u ? u->ft_type : 0);

        lua_pushnil(L);
        lua_pushliteral(L, "closed");
        return 2;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua udp socket read timeout: %M", u->read_timeout);

    u->waiting = 0;

    size = (size_t) luaL_optnumber(L, 2, UDP_MAX_DATAGRAM_SIZE);
    size = ngx_min(size, UDP_MAX_DATAGRAM_SIZE);

    u->recv_buf_size = size;

    dd("udp receive: buf size: %d", (int) u->recv_buf_size);

    rc = ngx_http_lua_socket_udp_read(r, u);

    if (rc == NGX_ERROR) {
        dd("read failed: %d", (int) u->ft_type);
        rc = ngx_http_lua_socket_udp_receive_retval_handler(r, u, L);
        dd("udp receive retval returned: %d", (int) rc);
        return rc;
    }

    if (rc == NGX_OK) {

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "lua udp socket receive done in a single run");

        return ngx_http_lua_socket_udp_receive_retval_handler(r, u, L);
    }

    /* n == NGX_AGAIN */

    u->read_event_handler = ngx_http_lua_socket_udp_read_handler;

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        return luaL_error(L, "no request ctx found");
    }

    if (ctx->entered_content_phase) {
        r->write_event_handler = ngx_http_lua_content_wev_handler;
    }

    u->waiting = 1;
    u->prepare_retvals = ngx_http_lua_socket_udp_receive_retval_handler;

    ctx->data = u;
    ctx->udp_socket_busy = 1;
    ctx->udp_socket_ready = 0;

    return lua_yield(L, 0);
}


static int
ngx_http_lua_socket_udp_receive_retval_handler(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u, lua_State *L)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua udp socket receive return value handler");

    if (u->ft_type) {
        return ngx_http_lua_socket_error_retval_handler(r, u, L);
    }

    lua_pushlstring(L, (char *) ngx_http_lua_socket_udp_buffer, u->received);
    return 1;
}


static int
ngx_http_lua_socket_udp_settimeout(lua_State *L)
{
    int                     n;
    ngx_int_t               timeout;

    ngx_http_lua_socket_udp_upstream_t  *u;

    n = lua_gettop(L);

    if (n != 2) {
        return luaL_error(L, "ngx.socket settimout: expecting at least 2 "
                          "arguments (including the object) but seen %d",
                          lua_gettop(L));
    }

    timeout = (ngx_int_t) lua_tonumber(L, 2);

    lua_rawseti(L, 1, SOCKET_TIMEOUT_INDEX);

    lua_rawgeti(L, 1, SOCKET_CTX_INDEX);
    u = lua_touserdata(L, -1);

    if (u) {
        if (timeout > 0) {
            u->read_timeout = (ngx_msec_t) timeout;

        } else {
            u->read_timeout = u->conf->read_timeout;
        }
    }

    return 0;
}


static void
ngx_http_lua_socket_udp_finalize(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua finalize socket");

    if (u->cleanup) {
        *u->cleanup = NULL;
        u->cleanup = NULL;
    }

    if (u->resolved && u->resolved->ctx) {
        ngx_resolve_name_done(u->resolved->ctx);
        u->resolved->ctx = NULL;
    }

    if (u->udp_connection.connection) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "lua close socket connection");

        ngx_close_connection(u->udp_connection.connection);
        u->udp_connection.connection = NULL;
    }
}


static int
ngx_http_lua_socket_udp_upstream_destroy(lua_State *L)
{
    ngx_http_lua_socket_udp_upstream_t      *u;

    dd("upstream destroy triggered by Lua GC");

    u = lua_touserdata(L, 1);
    if (u == NULL) {
        return 0;
    }

    if (u->cleanup) {
        ngx_http_lua_socket_udp_cleanup(u); /* it will clear u->cleanup */
    }

    return 0;
}


static void
ngx_http_lua_socket_dummy_handler(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua udp socket dummy handler");
}


static ngx_int_t
ngx_http_lua_socket_udp_read(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u)
{
    ngx_connection_t            *c;
    ngx_event_t                 *rev;
    ssize_t                      n;

    c = u->udp_connection.connection;
    rev = c->read;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "lua udp socket read data: waiting: %d", (int) u->waiting);

    n = ngx_udp_recv(u->udp_connection.connection,
                     ngx_http_lua_socket_udp_buffer, u->recv_buf_size);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "lua udp recv returned %z", n);

    if (n > 0) {
        u->received = n;
        ngx_http_lua_socket_udp_handle_success(r, u);
        return NGX_OK;
    }

    if (n == NGX_ERROR) {
        u->socket_errno = ngx_socket_errno;
        ngx_http_lua_socket_udp_handle_error(r, u,
                                             NGX_HTTP_LUA_SOCKET_FT_ERROR);
        return NGX_ERROR;
    }

    if (ngx_handle_read_event(rev, 0) != NGX_OK) {
        ngx_http_lua_socket_udp_handle_error(r, u,
                                             NGX_HTTP_LUA_SOCKET_FT_ERROR);
        return NGX_ERROR;
    }

    if (rev->active) {
        ngx_add_timer(rev, u->read_timeout);

    } else if (rev->timer_set) {
        ngx_del_timer(rev);
    }

    return NGX_AGAIN;
}


static void
ngx_http_lua_socket_udp_read_handler(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u)
{
    ngx_connection_t            *c;

    c = u->udp_connection.connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua udp socket read handler");

    if (c->read->timedout) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "lua udp socket read timed out");

        ngx_http_lua_socket_udp_handle_error(r, u,
                                             NGX_HTTP_LUA_SOCKET_FT_TIMEOUT);
        return;
    }

#if 1
    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }
#endif

    (void) ngx_http_lua_socket_udp_read(r, u);
}


static void
ngx_http_lua_socket_udp_handle_error(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u, ngx_uint_t ft_type)
{
    ngx_http_lua_ctx_t          *ctx;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua udp socket handle error");

    u->ft_type |= ft_type;

#if 0
    ngx_http_lua_socket_udp_finalize(r, u);
#endif

    u->read_event_handler = ngx_http_lua_socket_dummy_handler;

    if (u->waiting) {
        u->waiting = 0;

        ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);

        dd("setting socket_ready to 1");

        ctx->udp_socket_busy = 0;
        ctx->udp_socket_ready = 1;

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "lua udp socket waking up the current request");

        r->write_event_handler(r);
    }
}


static void
ngx_http_lua_socket_udp_cleanup(void *data)
{
    ngx_http_lua_socket_udp_upstream_t  *u = data;

    ngx_http_request_t  *r;

    r = u->request;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "cleanup lua udp socket upstream request: \"%V\"", &r->uri);

    ngx_http_lua_socket_udp_finalize(r, u);
}


static void
ngx_http_lua_socket_udp_handler(ngx_event_t *ev)
{
    ngx_connection_t                *c;
    ngx_http_request_t              *r;
    ngx_http_log_ctx_t              *ctx;

    ngx_http_lua_socket_udp_upstream_t  *u;

    c = ev->data;
    u = c->data;
    r = u->request;
    c = r->connection;

    ctx = c->log->data;
    ctx->current_request = r;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua udp socket handler for \"%V?%V\", wev %d", &r->uri,
                   &r->args, (int) ev->write);

    u->read_event_handler(r, u);

    ngx_http_run_posted_requests(c);
}


static void
ngx_http_lua_socket_udp_handle_success(ngx_http_request_t *r,
    ngx_http_lua_socket_udp_upstream_t *u)
{
    ngx_http_lua_ctx_t          *ctx;

    u->read_event_handler = ngx_http_lua_socket_dummy_handler;

    if (u->waiting) {
        u->waiting = 0;

        ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);

        dd("setting socket_ready to 1");

        ctx->udp_socket_busy = 0;
        ctx->udp_socket_ready = 1;

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "lua udp socket waking up the current request");

        r->write_event_handler(r);
    }
}
