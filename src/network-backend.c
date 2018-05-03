/* $%BEGINLICENSE%$
 Copyright (c) 2007, 2012, Oracle and/or its affiliates. All rights reserved.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation; version 2 of the
 License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA

 $%ENDLICENSE%$ */

#include "network-backend.h"

#include <string.h>
#include <glib.h>

#include "chassis-plugin.h"
#include "glib-ext.h"
#include "network-mysqld-proto.h"
#include "network-mysqld-packet.h"
#include "character-set.h"
#include "cetus-util.h"
#include "cetus-users.h"

const char *backend_state_t_str[] = {
    "unkown",
    "online",
    "down",
    "maintaining",
    "deleted"
};

const char *backend_type_t_str[] = {
    "unkown",
    "read/write",
    "readonly"
};

network_backend_t *
network_backend_new()
{
    network_backend_t *b;

    b = g_new0(network_backend_t, 1);

    b->pool = network_connection_pool_new();
    b->uuid = g_string_new(NULL);
    b->addr = network_address_new();
    b->server_group = g_string_new(NULL);
    b->address = g_string_new(NULL);
    b->challenges = g_ptr_array_new();

    return b;
}

void
network_backend_free(network_backend_t *b)
{
    if (!b)
        return;

    network_connection_pool_free(b->pool);

    if (b->addr)
        network_address_free(b->addr);
    if (b->uuid)
        g_string_free(b->uuid, TRUE);
    if (b->challenges)
        g_ptr_array_free(b->challenges, TRUE);
    if (b->server_group)
        g_string_free(b->server_group, TRUE);

    if (b->config) {
        if (b->config->default_username) {
            g_string_free(b->config->default_username, TRUE);
        }

        if (b->config->default_db) {
            g_string_free(b->config->default_db, TRUE);
        }

        g_free(b->config);
    }

    g_string_free(b->address, TRUE);
    g_debug("%s: call network_backend_free end", G_STRLOC);
    g_free(b);
}

int
network_backend_init_extra(network_backend_t *b, chassis *chas)
{
    if (chas->max_idle_connections != 0) {
        b->pool->max_idle_connections = chas->max_idle_connections;
    }

    if (chas->mid_idle_connections != 0) {
        b->pool->mid_idle_connections = chas->mid_idle_connections;
    }

    return 0;
}

int
network_backend_conns_count(network_backend_t *b)
{
    int in_use = b->connected_clients;
    int pooled = network_connection_pool_total_conns_count(b->pool);
    return in_use + pooled;
}

/*
 * save challenges from backend, will be used to authenticate front user
 */
void
network_backend_save_challenge(network_backend_t *b, const network_mysqld_auth_challenge *chal)
{
    if (b->challenges->len >= 1024) {
        network_mysqld_auth_challenge *challenge;
        challenge = g_ptr_array_remove_index(b->challenges, 0);
        network_mysqld_auth_challenge_free(challenge);
    }

    static const guint32 not_supported = CLIENT_LOCAL_FILES | CLIENT_DEPRECATE_EOF;

    network_mysqld_auth_challenge *challenge = network_mysqld_auth_challenge_copy(chal);

    challenge->capabilities &= ~not_supported;
#ifdef HAVE_OPENSSL
    challenge->capabilities |= CLIENT_SSL;
#else
    challenge->capabilities &= ~CLIENT_SSL;
#endif
    char *old_str = challenge->server_version_str;
    challenge->server_version_str = g_strdup_printf("%s (%s)", old_str, PACKAGE_STRING);
    g_free(old_str);

    g_ptr_array_add(b->challenges, challenge);
}

struct network_mysqld_auth_challenge *
network_backend_get_challenge(network_backend_t *b)
{
    if (b->challenges->len == 0) {
        g_message("challenges len 0 for backend:%s", b->addr->name->str);
        return NULL;
    }

    int ndx = g_random_int_range(0, 1024);
    ndx = ndx % b->challenges->len;

    network_mysqld_auth_challenge *challenge = g_ptr_array_index(b->challenges, ndx);
    return challenge;
}

static network_group_t *network_group_new();
static void network_group_free(network_group_t *);
static void network_group_add(network_group_t *, network_backend_t *);
static void network_group_update(network_group_t *gp);

network_backends_t *
network_backends_new()
{
    network_backends_t *bs;

    bs = g_new0(network_backends_t, 1);

    bs->backends = g_ptr_array_new();
    bs->groups = g_ptr_array_new_with_free_func((GDestroyNotify) network_group_free);
    return bs;
}

void
network_backends_free(network_backends_t *bs)
{
    gsize i, j;

    if (!bs)
        return;

    g_message("%s: call network_backends_free", G_STRLOC);

    for (i = 0; i < bs->backends->len; i++) {
        network_backend_t *backend = bs->backends->pdata[i];

        for (j = 0; j < backend->challenges->len; j++) {
            network_mysqld_auth_challenge *challenge = backend->challenges->pdata[j];
            network_mysqld_auth_challenge_free(challenge);
        }

        network_backend_free(backend);
    }
    g_ptr_array_free(bs->backends, TRUE);
    g_ptr_array_free(bs->groups, TRUE);
    g_free(bs);
}

static gboolean
network_backends_into_group(network_backends_t *bs, network_backend_t *backend)
{
    if (backend && backend->server_group) {
        network_group_t *gp;
        if ((gp = network_backends_get_group(bs, backend->server_group)) != NULL) {
            network_group_add(gp, backend);
            return TRUE;
        }
    }
    return FALSE;
}

static void network_backends_add_group(network_backends_t *bs, const char *name);

static void set_backend_config(network_backend_t *backend, chassis *srv) {
    if (!backend->config) {
        backend->config = g_new0(backend_config, 1);
    } else {
        if (backend->config->default_username) {
            g_string_free(backend->config->default_username, TRUE);
        }

        if (backend->config->default_db) {
            g_string_free(backend->config->default_db, TRUE);
        }
    }

    backend->config->default_username = g_string_new(NULL);
    g_string_append(backend->config->default_username, srv->default_username);

    if (srv->default_db != NULL && strlen(srv->default_db) > 0) {
        backend->config->default_db = g_string_new(NULL);
        g_string_append(backend->config->default_db, srv->default_db);
    }

    backend->config->charset = charset_get_number(srv->default_charset);

    backend->config->mid_conn_pool = srv->mid_idle_connections;
    backend->config->max_conn_pool = srv->max_idle_connections;
}

/*
 * FIXME: 1) remove _set_address, make this function callable with result of same
 *        2) differentiate between reasons for "we didn't add" (now -1 in all cases)
 */
int
network_backends_add(network_backends_t *bs, const gchar *address,
                     backend_type_t type, backend_state_t state, void *srv)
{
    network_backend_t *new_backend = network_backend_new();
    new_backend->type = type;
    new_backend->state = state;
    new_backend->pool->srv = srv;

    char *group_p = NULL;
    if ((group_p = strrchr(address, '@')) != NULL) {
        network_backends_add_group(bs, group_p + 1);
        g_string_assign(new_backend->server_group, group_p + 1);
        g_string_assign_len(new_backend->address, address, group_p - address);
    } else {
        g_string_assign(new_backend->address, address);
    }

    if (0 != network_address_set_address(new_backend->addr, new_backend->address->str)) {
        network_backend_free(new_backend);
        return -1;
    }

    guint i;
    /* check if this backend is already known */
    for (i = 0; i < bs->backends->len; i++) {
        network_backend_t *old_backend = g_ptr_array_index(bs->backends, i);

        if (strleq(S(old_backend->addr->name), S(new_backend->addr->name))) {
            network_backend_free(new_backend);

            g_critical("backend %s is already known!", address);
            return -1;
        }
    }

    g_ptr_array_add(bs->backends, new_backend);
    if (type == BACKEND_TYPE_RO) {
        bs->ro_server_num += 1;
    }

    set_backend_config(new_backend, srv);
    network_backends_into_group(bs, new_backend);
    g_message("added %s backend: %s, state: %s", backend_type_t_str[type], address, backend_state_t_str[state]);

    return 0;
}

/**
 * we just change the state to deleted
 */
int
network_backends_remove(network_backends_t *bs, guint index)
{
    network_backend_t *b = bs->backends->pdata[index];
    if (b != NULL) {
        if (b->type == BACKEND_TYPE_RO && bs->ro_server_num > 0) {
            bs->ro_server_num -= 1;
        }

        return network_backends_modify(bs, index, BACKEND_TYPE_UNKNOWN, BACKEND_STATE_DELETED);
    }
    return 0;
}

/**
 * updated the _DOWN state to _UNKNOWN if the backends were
 * down for at least 4 seconds
 *
 * we only check once a second to reduce the overhead on connection setup
 *
 * @returns   number of updated backends
 */
int
network_backends_check(network_backends_t *bs)
{
    GTimeVal now;
    guint i;
    int backends_woken_up = 0;
    gint64 t_diff;

    g_get_current_time(&now);
    ge_gtimeval_diff(&bs->backend_last_check, &now, &t_diff);

    /* check max(once a second) */
    /* this also covers the "time went backards" case */
    if (t_diff < G_USEC_PER_SEC) {
        if (t_diff < 0) {
            g_message("%s: time went backwards (%" G_GINT64_FORMAT " usec)!", G_STRLOC, t_diff);
            bs->backend_last_check.tv_usec = 0;
            bs->backend_last_check.tv_sec = 0;
        }
        return 0;
    }

    bs->backend_last_check = now;

    for (i = 0; i < bs->backends->len; i++) {
        network_backend_t *cur = bs->backends->pdata[i];

        if (cur->state != BACKEND_STATE_DOWN)
            continue;

        /* check if a backend is marked as down for more than 4 sec */
        if (now.tv_sec - cur->state_since.tv_sec > 4) {
            g_debug("%s: backend %s was down for more than 4 secs, waking it up", G_STRLOC, cur->addr->name->str);

            cur->state = BACKEND_STATE_UNKNOWN;
            cur->state_since = now;
            backends_woken_up++;
        }
    }

    return backends_woken_up;
}

/**
 * modify the backends to new type and new state.
 *
 * @returns   0 for success -1 for error.
 */

int
network_backends_modify(network_backends_t *bs, guint ndx, backend_type_t type, backend_state_t state)
{
    GTimeVal now;
    g_get_current_time(&now);
    if (ndx >= network_backends_count(bs))
        return -1;
    network_backend_t *cur = bs->backends->pdata[ndx];

    g_message("change backend: %s from type: %s, state: %s to type: %s, state: %s",
              cur->addr->name->str, backend_type_t_str[cur->type],
              backend_state_t_str[cur->state], backend_type_t_str[type], backend_state_t_str[state]);

    if (cur->type != type) {
        cur->type = type;
        if (type == BACKEND_TYPE_RO) {
            bs->ro_server_num += 1;
        } else {
            bs->ro_server_num -= 1;
        }
        network_group_t *gp = network_backends_get_group(bs, cur->server_group);
        if (gp)
            network_group_update(gp);
    }
    if (cur->state != state) {
        cur->state = state;
        cur->state_since = now;
    }

    g_debug("%s: backend state:%d for backend:%p", G_STRLOC, cur->state, cur);

    return 0;
}

network_backend_t *
network_backends_get(network_backends_t *bs, guint ndx)
{
    if (ndx >= network_backends_count(bs))
        return NULL;

    /* FIXME: shouldn't we copy the backend or add ref-counting ? */
    return bs->backends->pdata[ndx];
}

guint
network_backends_count(network_backends_t *bs)
{
    guint len;

    len = bs->backends->len;

    return len;
}

#define DEFAULT_CHARSET   '\x21'


gboolean
network_backends_load_config(network_backends_t *bs, chassis *srv)
{
    if (!cetus_users_contains(srv->priv->users, srv->default_username)) {
        g_critical("%s: no required password here for user:%s", G_STRLOC, srv->default_username);
        return -1;
    }
    int i;
    for (i = 0; i < network_backends_count(bs); i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        if (backend) {
            set_backend_config(backend, srv);
        }
    }
    return 0;
}

network_group_t *
network_backends_get_group(network_backends_t *bs, const GString *name)
{
    int i = 0;
    for (i = 0; i < bs->groups->len; ++i) {
        network_group_t *group = g_ptr_array_index(bs->groups, i);
        if (g_string_equal(group->name, name)) {
            return group;
        }
    }
    return NULL;
}

static void
network_backends_add_group(network_backends_t *bs, const char *name)
{
    GString *gp_name = g_string_new(name);
    if (!network_backends_get_group(bs, gp_name)) { /* dup check */
        network_group_t *gp = network_group_new(gp_name);
        g_ptr_array_add(bs->groups, gp);
    } else {
        g_string_free(gp_name, TRUE);
    }
}

static network_group_t *
network_group_new(GString *name)
{
    network_group_t *gp = g_new0(network_group_t, 1);
    gp->name = name;
    return gp;
}

static void
network_group_free(network_group_t *gp)
{
    g_string_free(gp->name, TRUE);
    g_free(gp);
}

static void
network_group_add(network_group_t *gp, network_backend_t *backend)
{
    g_assert(backend);
    if (backend->type == BACKEND_TYPE_RW) {
        if (gp->master) {
            g_critical("only one master each group");
        }
        gp->master = backend;
    } else if (backend->type == BACKEND_TYPE_RO) {
        if (gp->nslaves >= MAX_GROUP_SLAVES) {
            g_critical("too many slaves for group");
            return;
        }
        int i = 0;
        for (i = 0; i < gp->nslaves; ++i) {
            network_backend_t *slave = gp->slaves[i];
            if (strleq(S(slave->addr->name), S(backend->addr->name))) {
                return;
            }
        }
        gp->nslaves += 1;
        gp->slaves[gp->nslaves - 1] = backend;
    }
}

/* some backend in this group changed rw type, update */
static void
network_group_update(network_group_t *gp)
{
    /* take out backends of this group evenly */
    GList *backends = NULL;
    if (gp->master) {
        backends = g_list_append(backends, gp->master);
        gp->master = NULL;
    }
    int i;
    for (i = 0; i < gp->nslaves; ++i) {
        backends = g_list_append(backends, gp->slaves[i]);
    }
    gp->nslaves = 0;

    /* rearrange them into this group */
    GList *l;
    for (l = backends; l != NULL; l = l->next) {
        network_group_add(gp, l->data);
    }
    g_list_free(backends);
}

static int
backends_get_ro_ndx_round_robin(network_backends_t *bs)
{
    int ro_index = 0, remainder = 0;
    if (bs->ro_server_num > 0) {
        remainder = (bs->read_count++) % bs->ro_server_num;
    }
    int count = network_backends_count(bs);
    int i = 0;
    for (i = 0; i < count; i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        if ((backend->type == BACKEND_TYPE_RO)
            && (backend->state == BACKEND_STATE_UP || backend->state == BACKEND_STATE_UNKNOWN)) {
            if (ro_index == remainder) {
                break;
            }
            ro_index++;
        }
    }
    return i < count ? i : -1;
}

static int
backends_get_ro_ndx_first(network_backends_t *bs)
{
    int i = 0;
    int count = network_backends_count(bs);
    for (i = 0; i < count; i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        if ((backend->type == BACKEND_TYPE_RO)
            && (backend->state == BACKEND_STATE_UP || backend->state == BACKEND_STATE_UNKNOWN)) {
            return i;
        }
    }
    return -1;
}

static int
backends_get_ro_ndx_random(network_backends_t *bs)
{
    int count = network_backends_count(bs);
    int ndx = g_random_int_range(0, count);
    network_backend_t *backend = network_backends_get(bs, ndx);
    if (backend->type == BACKEND_TYPE_RO) { /* luckily run into a RO */
        if (backend->state == BACKEND_STATE_UP || backend->state == BACKEND_STATE_UNKNOWN) {
            return ndx;
        }
    } else {                    /* if we run into a RW, try it's neighbours */
        if (ndx - 1 >= 0) {
            backend = network_backends_get(bs, ndx - 1);
            if ((backend->type == BACKEND_TYPE_RO)
                && (backend->state == BACKEND_STATE_UP || backend->state == BACKEND_STATE_UNKNOWN)) {
                return ndx - 1;
            }
        } else if (ndx + 1 < count) {
            if ((backend->type == BACKEND_TYPE_RO)
                && (backend->state == BACKEND_STATE_UP || backend->state == BACKEND_STATE_UNKNOWN)) {
                return ndx + 1;
            }
        }
    }

    return backends_get_ro_ndx_first(bs);
}

int
network_backends_get_ro_ndx(network_backends_t *bs, backend_algo_t algo)
{
    switch (algo) {
    case BACKEND_ALGO_ROUND_ROBIN:
        return backends_get_ro_ndx_round_robin(bs);
    case BACKEND_ALGO_RANDOM:
        return backends_get_ro_ndx_random(bs);
    case BACKEND_ALGO_FIRST:
        return backends_get_ro_ndx_first(bs);
    default:
        return -1;
    }
}

int
network_backends_get_rw_ndx(network_backends_t *bs)
{
    int i = 0;
    int count = network_backends_count(bs);
    for (i = 0; i < count; i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        if ((BACKEND_TYPE_RW == backend->type) &&
            (backend->state == BACKEND_STATE_UP || backend->state == BACKEND_STATE_UNKNOWN)) {
            break;
        }
    }
    return i < count ? i : -1;
}

int
network_backends_find_address(network_backends_t *bs, const char *ipport)
{
    int count = network_backends_count(bs);
    int i = 0;
    for (i = 0; i < count; i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        if (strcmp(backend->addr->name->str, ipport) == 0) {
            return i;
        }
    }
    return -1;
}

network_mysqld_auth_challenge *
network_backends_get_challenge(network_backends_t *bs, int back_ndx)
{
    network_backend_t *b = network_backends_get(bs, back_ndx);
    if (b)
        return network_backend_get_challenge(b);
    else
        return NULL;
}

/* round robin pick */
network_backend_t *
network_group_pick_slave_backend(network_group_t *group)
{
    int i;
    network_backend_t *backend = NULL;
    for (i = 0; i < group->nslaves; i++) {
        size_t index = (group->slave_visit_cnt++) % group->nslaves;
        backend = group->slaves[index];
        if (backend->state != BACKEND_STATE_UP && backend->state != BACKEND_STATE_UNKNOWN) {
            g_debug(G_STRLOC ": skip dead backend(slave): %d", i);
            continue;
        }

        int total = network_backend_conns_count(backend);
        int connected_clts = backend->connected_clients;
        int cur_idle = total - connected_clts;
        int max_idle_conns = backend->config->max_conn_pool;

        g_debug("%s, slave:%d, total:%d, connected:%d, idle:%d, max:%d",
                G_STRLOC, (int)i, total, connected_clts, cur_idle, max_idle_conns);

        if (cur_idle || total <= max_idle_conns) {
            break;
        }
    }
    if (i == group->nslaves) {
        backend = NULL;
    }
    return backend;
}

void
network_group_get_slave_names(network_group_t *group, GString *slaves)
{
    int i;
    for (i = 0; i < group->nslaves; ++i) {
        network_backend_t *b = group->slaves[i];
        g_string_append(slaves, b->addr->name->str);
        g_string_append_c(slaves, ' ');
    }
}

int
network_backends_idle_conns(network_backends_t *bs)
{
    int sum = 0;
    int count = network_backends_count(bs);
    int i;
    for (i = 0; i < count; i++) {
        network_backend_t *b = network_backends_get(bs, i);
        int pooled = network_connection_pool_total_conns_count(b->pool);
        sum += pooled;
    }
    return sum;
}

int
network_backends_used_conns(network_backends_t *bs)
{
    int sum = 0;
    int count = network_backends_count(bs);
    int i;
    for (i = 0; i < count; i++) {
        network_backend_t *b = network_backends_get(bs, i);
        int in_use = b->connected_clients;
        sum += in_use;
    }
    return sum;
}
