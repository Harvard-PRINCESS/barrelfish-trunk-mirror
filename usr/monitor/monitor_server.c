/** \file
 * \brief Monitor's connection with the dispatchers on the same core
 */

/*
 * Copyright (c) 2009, 2010, 2011, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include "monitor.h"
#include <barrelfish/debug.h> // XXX: To set the cap_identify_reply handler
#include <barrelfish/sys_debug.h> // XXX: for sys_debug_send_ipi
#include <trace/trace.h>
#include <if/mem_defs.h>
#include <barrelfish/monitor_client.h>
#include <if/monitor_loopback_defs.h>
#include "capops.h"

// the monitor's loopback binding to itself
static struct monitor_binding monitor_self_binding;

/* ---------------------- MULTIBOOT REQUEST CODE START ---------------------- */

struct multiboot_cap_state {
    struct monitor_msg_queue_elem elem;
    cslot_t slot;
};

static void ms_multiboot_cap_request(struct monitor_binding *b, cslot_t slot);

static void ms_multiboot_cap_request_handler(struct monitor_binding *b,
                                             struct monitor_msg_queue_elem *e)
{
    struct multiboot_cap_state *ms = (struct multiboot_cap_state*)e;
    ms_multiboot_cap_request(b, ms->slot);
    free(ms);
}

static void ms_multiboot_cap_request(struct monitor_binding *b, cslot_t slot)
{
    errval_t err1, err2;

    struct capref cap = {
        .cnode = cnode_module,
        .slot  = slot,
    };

    // Call frame_identify to check if cap exists
    struct frame_identity id;
    err1 = invoke_frame_identify(cap, &id);
    if (err_is_fail(err1)) {
        err2 = b->tx_vtbl.multiboot_cap_reply(b, NOP_CONT, NULL_CAP, err1);
    } else {
        err2 = b->tx_vtbl.multiboot_cap_reply(b, NOP_CONT, cap, err1);
    }
    if (err_is_fail(err2)) {
        if (err_no(err2) == FLOUNDER_ERR_TX_BUSY) {
            struct monitor_state *mon_state = b->st;
            struct multiboot_cap_state *ms =
                malloc(sizeof(struct multiboot_cap_state));
            assert(ms);
            ms->slot = slot;
            ms->elem.cont = ms_multiboot_cap_request_handler;
            err1 = monitor_enqueue_send(b, &mon_state->queue,
                                       get_default_waitset(), &ms->elem.queue);
            if (err_is_fail(err1)) {
                USER_PANIC_ERR(err1, "monitor_enqueue_send failed");
            }
        } else {
            USER_PANIC_ERR(err2, "sending multiboot_cap_reply failed");
        }
    }
}

/* ----------------------- MULTIBOOT REQUEST CODE END ----------------------- */

static void alloc_iref_request(struct monitor_binding *b,
                               uintptr_t service_id)
{
    errval_t err, reterr;

    iref_t iref = 0;
    reterr = iref_alloc(b, service_id, &iref);
    err = b->tx_vtbl.alloc_iref_reply(b, NOP_CONT, service_id, iref, reterr);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "reply failed");
    }
}

/******* stack-ripped bind_lmp_service_request *******/

static void bind_lmp_client_request_error_handler(struct monitor_binding *b,
                                                  struct monitor_msg_queue_elem *e);

struct bind_lmp_client_request_error_state {
    struct monitor_msg_queue_elem elem;
    struct monitor_bind_lmp_reply_client__args args;
    struct monitor_binding *serv_binding;
    struct capref ep;
};

static void bind_lmp_client_request_error(struct monitor_binding *b,
                                          errval_t err, uintptr_t domain_id,
                                          struct monitor_binding *serv_binding,
                                          struct capref ep)
{
    errval_t err2;

    err2 = b->tx_vtbl.bind_lmp_reply_client(b, NOP_CONT, err, 0, domain_id,
                                            NULL_CAP);
    if (err_is_fail(err2)) {
        if(err_no(err2) == FLOUNDER_ERR_TX_BUSY) {
            struct bind_lmp_client_request_error_state *me =
                malloc(sizeof(struct bind_lmp_client_request_error_state));
            assert(me != NULL);
            struct monitor_state *ist = b->st;
            assert(ist != NULL);
            me->args.err = err;
            me->args.conn_id = domain_id;
            me->serv_binding = serv_binding;
            me->ep = ep;
            me->elem.cont = bind_lmp_client_request_error_handler;

            err = monitor_enqueue_send(b, &ist->queue,
                                       get_default_waitset(), &me->elem.queue);
            if (err_is_fail(err)) {
                USER_PANIC_ERR(err, "monitor_enqueue_send failed");
            }
            return;
        }

        USER_PANIC_ERR(err2, "error reply failed");
        USER_PANIC_ERR(err, "The reason for lmp failure");
    }

    /* Delete the EP cap */
    // Do not delete the cap if client or service is monitor itself
    if (b != &monitor_self_binding && serv_binding != &monitor_self_binding) {
        err = cap_destroy(ep);
        if (err_is_fail(err)) {
            USER_PANIC_ERR(err, "cap_destroy failed");
        }
    }
}

static void bind_lmp_client_request_error_handler(struct monitor_binding *b,
                                                  struct monitor_msg_queue_elem *e)
{
    struct bind_lmp_client_request_error_state *st = (struct bind_lmp_client_request_error_state *)e;
    bind_lmp_client_request_error(b, st->args.err, st->args.conn_id,
                                  st->serv_binding, st->ep);
    free(e);
}

static void bind_lmp_service_request_handler(struct monitor_binding *b,
                                             struct monitor_msg_queue_elem *e);

struct bind_lmp_service_request_state {
    struct monitor_msg_queue_elem elem;
    struct monitor_bind_lmp_service_request__args args;
    struct monitor_binding *b;
    uintptr_t domain_id;
};

static void bind_lmp_service_request_cont(struct monitor_binding *serv_binding,
                                          uintptr_t service_id, uintptr_t con_id,
                                          size_t buflen, struct capref ep,
                                          struct monitor_binding *b,
                                          uintptr_t domain_id)
{
    errval_t err, err2;

    struct monitor_state *ist = serv_binding->st;
    struct event_closure send_cont = NOP_CONT;
    struct capref *capp = NULL;

    if (serv_binding != &monitor_self_binding && b != &monitor_self_binding) {
        // save EP cap to be destroyed after the send is done
        capp = caprefdup(ep);
        send_cont = MKCONT(destroy_outgoing_cap, capp);
    }

    err = serv_binding->tx_vtbl.
        bind_lmp_service_request(serv_binding, send_cont, service_id,
                                 con_id, buflen, ep);
    if (err_is_fail(err)) {
        free(capp);

        if(err_no(err) == FLOUNDER_ERR_TX_BUSY) {
            struct bind_lmp_service_request_state *me =
                malloc(sizeof(struct bind_lmp_service_request_state));
            assert(me != NULL);
            me->args.service_id = service_id;
            me->args.mon_id = con_id;
            me->args.buflen = buflen;
            me->args.ep = ep;
            me->b = b;
            me->domain_id = domain_id;
            me->elem.cont = bind_lmp_service_request_handler;

            err = monitor_enqueue_send(serv_binding, &ist->queue,
                                       get_default_waitset(), &me->elem.queue);
            if (err_is_fail(err)) {
                USER_PANIC_ERR(err, "monitor_enqueue_send failed");
            }
            return;
        }

        err2 = lmp_conn_free(con_id);
        if (err_is_fail(err2)) {
            USER_PANIC_ERR(err2, "lmp_conn_free failed");
        }
        bind_lmp_client_request_error(b, err, domain_id, serv_binding, ep);
        return;
    }
}

static void bind_lmp_service_request_handler(struct monitor_binding *b,
                                             struct monitor_msg_queue_elem *e)
{
    struct bind_lmp_service_request_state *st = (struct bind_lmp_service_request_state *)e;
    bind_lmp_service_request_cont(b, st->args.service_id, st->args.mon_id,
                                  st->args.buflen, st->args.ep, st->b,
                                  st->domain_id);
    free(e);
}

static void bind_lmp_client_request(struct monitor_binding *b,
                                    iref_t iref, uintptr_t domain_id,
                                    size_t buflen, struct capref ep)
{
    errval_t err;
    struct monitor_binding *serv_binding = NULL;

    /* Look up core_id from the iref */
    uint8_t core_id;
    err = iref_get_core_id(iref, &core_id);
    if (err_is_fail(err)) {
        bind_lmp_client_request_error(b, err, domain_id, serv_binding, ep);
        return;
    }

    // Return error if service on different core
    if (core_id != my_core_id) {
        err = MON_ERR_IDC_BIND_NOT_SAME_CORE;
        bind_lmp_client_request_error(b, err, domain_id, serv_binding, ep);
        return;
    }

    /* Lookup the server's connection to monitor */
    err = iref_get_binding(iref, &serv_binding);
    if (err_is_fail(err)) {
        bind_lmp_client_request_error(b, err, domain_id, serv_binding, ep);
        return;
    }

    /* Lookup the server's service_id */
    uintptr_t service_id;
    err = iref_get_service_id(iref, &service_id);
    if (err_is_fail(err)) {
        bind_lmp_client_request_error(b, err, domain_id, serv_binding, ep);
        return;
    }

    /* Allocate a new monitor connection */
    uintptr_t con_id;
    struct lmp_conn_state *conn;
    err = lmp_conn_alloc(&conn, &con_id);
    if (err_is_fail(err)) {
        bind_lmp_client_request_error(b, err, domain_id, serv_binding, ep);
        return;
    }

    conn->domain_id = domain_id;
    conn->domain_binding = b;

    /* Send request to the server */
    bind_lmp_service_request_cont(serv_binding, service_id, con_id, buflen, ep,
                                  b, domain_id);
}

/******* stack-ripped bind_lmp_reply *******/

static void bind_lmp_reply_client_handler(struct monitor_binding *b,
                                          struct monitor_msg_queue_elem *e);

struct bind_lmp_reply_client_state {
    struct monitor_msg_queue_elem elem;
    struct monitor_bind_lmp_reply_client__args args;
    struct monitor_binding *b;
};

static void bind_lmp_reply_client_cont(struct monitor_binding *client_binding,
                                       errval_t msgerr, uintptr_t mon_conn_id,
                                       uintptr_t client_conn_id,
                                       struct capref ep,
                                       struct monitor_binding *b)
{
    errval_t err;

    struct monitor_state *ist = client_binding->st;
    struct event_closure send_cont = NOP_CONT;
    struct capref *capp = NULL;

    if (client_binding != &monitor_self_binding && b != &monitor_self_binding) {
        // save EP cap to be destroyed after the send is done
        capp = caprefdup(ep);
        send_cont = MKCONT(destroy_outgoing_cap, capp);
    }

    err = client_binding->tx_vtbl.
        bind_lmp_reply_client(client_binding, send_cont,
                              SYS_ERR_OK, mon_conn_id, client_conn_id, ep);
    if (err_is_fail(err)) {
        free(capp);

        if(err_no(err) == FLOUNDER_ERR_TX_BUSY) {
            struct bind_lmp_reply_client_state *me =
                malloc(sizeof(struct bind_lmp_reply_client_state));
            assert(me != NULL);
            me->args.err = msgerr;
            me->args.mon_id = mon_conn_id;
            me->args.conn_id = client_conn_id;
            me->args.ep = ep;
            me->b = b;
            me->elem.cont = bind_lmp_reply_client_handler;

            err = monitor_enqueue_send(client_binding, &ist->queue,
                                       get_default_waitset(), &me->elem.queue);
            if (err_is_fail(err)) {
                USER_PANIC_ERR(err, "monitor_enqueue_send failed");
            }
            return;
        }

        USER_PANIC_ERR(err, "failed sending IDC bind reply");
    }

    if(err_is_fail(msgerr)) {
        return;
    }
}

static void bind_lmp_reply_client_handler(struct monitor_binding *b,
                                          struct monitor_msg_queue_elem *e)
{
    struct bind_lmp_reply_client_state *st = (struct bind_lmp_reply_client_state *)e;
    bind_lmp_reply_client_cont(b, st->args.err, st->args.mon_id, st->args.conn_id,
                               st->args.ep, st->b);
    free(e);
}

static void bind_lmp_reply(struct monitor_binding *b,
                           errval_t msgerr, uintptr_t mon_conn_id,
                           uintptr_t user_conn_id, struct capref ep)
{
    errval_t err;
    struct monitor_binding *client_binding = NULL;

    struct lmp_conn_state *conn = lmp_conn_lookup(mon_conn_id);
    if (conn == NULL) {
        DEBUG_ERR(0, "invalid connection ID");
        goto cleanup;
    }

    client_binding = conn->domain_binding;
    uintptr_t client_conn_id = conn->domain_id;

    err = lmp_conn_free(mon_conn_id);
    assert(err_is_ok(err));

    if (err_is_fail(msgerr)) {
        bind_lmp_reply_client_cont(client_binding, msgerr, 0, client_conn_id,
                                   ep, b);
    } else {
        bind_lmp_reply_client_cont(client_binding, SYS_ERR_OK, mon_conn_id,
                                   client_conn_id, ep, b);
    }
    return;

cleanup:
    /* Delete the ep cap */
    // XXX: Do not delete the cap if client or service is monitor
    if (client_binding != &monitor_self_binding && b != &monitor_self_binding) {
        err = cap_destroy(ep);
        if (err_is_fail(err)) {
            USER_PANIC_ERR(err, "cap_destroy failed");
        }
    }
}

/* ---------------------- NEW MONITOR BINDING CODE START -------------------- */

struct new_monitor_binding_reply_state {
    struct monitor_msg_queue_elem elem;
    struct monitor_new_monitor_binding_reply__args args;
};

static void
new_monitor_binding_reply_cont(struct monitor_binding *b,
                               errval_t reterr, struct capref retcap,
                               uintptr_t st);

static void new_monitor_binding_reply_handler(struct monitor_binding *b,
                                              struct monitor_msg_queue_elem *e)
{
    struct new_monitor_binding_reply_state *st =
        (struct new_monitor_binding_reply_state *)e;
    new_monitor_binding_reply_cont(b, st->args.err, st->args.ep, st->args.st);
    free(st);
}

static void
new_monitor_binding_reply_cont(struct monitor_binding *b,
                               errval_t reterr, struct capref retcap,
                               uintptr_t st)
{
    errval_t err =
        b->tx_vtbl.new_monitor_binding_reply(b, NOP_CONT, reterr, retcap, st);

    if (err_is_fail(err)) {
        if (err_no(err) == FLOUNDER_ERR_TX_BUSY) {
            struct monitor_state *ms = b->st;
            struct new_monitor_binding_reply_state *me =
                malloc(sizeof(struct new_monitor_binding_reply_state));
            assert(me != NULL);
            me->args.err = reterr;
            me->args.ep = retcap;
            me->args.st = st;
            me->elem.cont = new_monitor_binding_reply_handler;
            err = monitor_enqueue_send(b, &ms->queue,
                                       get_default_waitset(), &me->elem.queue);
            if (err_is_fail(err)) {
                USER_PANIC_ERR(err, "monitor_enqueue_send failed");
            }
            return;
        }

        USER_PANIC_ERR(err, "failed to send new_monitor_binding_reply");
    }
}

/**
 * \brief Setup a new idc channel between monitor and domain
 *
 * \bug on error send message back to domain
 */
static void new_monitor_binding_request(struct monitor_binding *b, uintptr_t st)
{
    struct capref retcap = NULL_CAP;
    errval_t err, reterr = SYS_ERR_OK;

    struct monitor_lmp_binding *lmpb =
        malloc(sizeof(struct monitor_lmp_binding));
    assert(lmpb != NULL);

    // setup our end of the binding
    err = monitor_client_lmp_accept(lmpb, get_default_waitset(),
                                    DEFAULT_LMP_BUF_WORDS);
    if (err_is_fail(err)) {
        free(lmpb);
        reterr = err_push(err, LIB_ERR_MONITOR_CLIENT_ACCEPT);
        goto out;
    }

    retcap = lmpb->chan.local_cap;
    monitor_server_init(&lmpb->b);

out:
    new_monitor_binding_reply_cont(b, reterr, retcap, st);
}

/* ---------------------- NEW MONITOR BINDING CODE END ---------------------- */

static void get_mem_iref_request(struct monitor_binding *b)
{
    errval_t err;

    // Mem serv not registered yet
    assert(mem_serv_iref != 0);

    err = b->tx_vtbl.get_mem_iref_reply(b, NOP_CONT, mem_serv_iref);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "reply failed");
    }
}

static void get_name_iref_request(struct monitor_binding *b, uintptr_t st)
{
    errval_t err;
    err = b->tx_vtbl.get_name_iref_reply(b, NOP_CONT, name_serv_iref, st);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "reply failed");
    }
}

static void get_ramfs_iref_request(struct monitor_binding *b, uintptr_t st)
{
    errval_t err;
    err = b->tx_vtbl.get_ramfs_iref_reply(b, NOP_CONT, ramfs_serv_iref, st);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "reply failed");
    }
}

static void set_mem_iref_request(struct monitor_binding *b, 
                                 iref_t iref)
{
    mem_serv_iref = iref;
    update_ram_alloc_binding = true;
}

static void get_monitor_rpc_iref_request(struct monitor_binding *b, 
                                         uintptr_t st_arg)
{
    errval_t err;

    if (monitor_rpc_iref == 0) {
        // Monitor rpc not registered yet
        DEBUG_ERR(LIB_ERR_GET_MON_BLOCKING_IREF, "got monitor rpc iref request but iref is 0");
    }

    err = b->tx_vtbl.get_monitor_rpc_iref_reply(b, NOP_CONT,
                                                monitor_rpc_iref, st_arg);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "reply failed");
    }
}


void set_monitor_rpc_iref(iref_t iref)
{
    if (monitor_rpc_iref != 0) {
        // Called multiple times, return error
        DEBUG_ERR(0, "Attempt to reset monitor rpc IREF ignored");
        return;
    }

    monitor_rpc_iref = iref;
}


static void set_name_iref_request(struct monitor_binding *b, 
                                  iref_t iref)
{
    if (name_serv_iref != 0) {
        // Called multiple times, return error
        DEBUG_ERR(0, "Attempt to reset name serv IREF ignored");
        return;
    }

    name_serv_iref = iref;
}

static void set_ramfs_iref_request(struct monitor_binding *b,
                                  iref_t iref)
{
    if (ramfs_serv_iref != 0) {
        // Called multiple times, return error
        DEBUG_ERR(0, "Attempt to reset name serv IREF ignored");
        return;
    }

    ramfs_serv_iref = iref;
}

struct send_cap_st {
    struct intermon_msg_queue_elem qe; // must be first
    uintptr_t my_mon_id;
    struct capref cap;
    uint32_t capid;
    uint8_t give_away;
    capaddr_t cptr;
    uint8_t vbits;
    cslot_t slot;
};

static void
cap_send_tx_cont(struct intermon_binding *b,
                        struct intermon_msg_queue_elem *e)
{
    errval_t send_err;
    struct send_cap_st *st = (struct send_cap_st*)e;
    struct remote_conn_state *conn = remote_conn_lookup(st->my_mon_id);
    send_err = intermon_cap_send_request__tx(b, NOP_CONT, conn->mon_id,
                                                st->capid, st->cptr,
                                                st->vbits, st->slot);
    if (err_is_fail(send_err)) {
        DEBUG_ERR(send_err, "sending cap_send_request failed");
    }
}

static void
cap_send_delete_cont(errval_t status, void *st)
{
    errval_t queue_err;
    struct send_cap_st *send_st = (struct send_cap_st*)st;

    if (err_is_fail(status)) {
        DEBUG_ERR(status, "delete for cap_send_request failed");
        return;
    }

    send_st->qe.cont = cap_send_tx_cont;
    struct remote_conn_state *conn = remote_conn_lookup(send_st->my_mon_id);
    struct intermon_binding *binding = conn->mon_binding;
    struct intermon_state *inter_st = (struct intermon_state*)binding->st;
    queue_err = intermon_enqueue_send(binding, &inter_st->queue,
                                      binding->waitset,
                                      (struct msg_queue_elem*)send_st);
    if (err_is_fail(queue_err)) {
        DEBUG_ERR(queue_err, "enqueuing cap_send_request failed");
    }
}

static void
cap_send_copy_cont(errval_t status, capaddr_t cptr, uint8_t vbits,
                   cslot_t slot, void *st)
{
    errval_t err;

    struct send_cap_st *send_st = (struct send_cap_st*)st;
    if (err_is_fail(status)) {
        DEBUG_ERR(status, "copy for cap_send_request failed");
        return;
    }

    send_st->cptr = cptr;
    send_st->vbits = vbits;
    send_st->slot = slot;

    err = capops_delete(get_cap_domref(send_st->cap), cap_send_delete_cont, st);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "starting delete for cap_send_request failed");
    }
}

static void
cap_send_request(struct monitor_binding *b, uintptr_t my_mon_id,
                 struct capref cap, uint32_t capid)
{
    errval_t err;
    struct remote_conn_state *conn = remote_conn_lookup(my_mon_id);
    debug_printf("cap_send_request\n");

    struct send_cap_st *st;
    st = calloc(1, sizeof(*st));
    if (!st) {
        DEBUG_ERR(LIB_ERR_MALLOC_FAIL, "Failed to allocate cap_send_request state");
        return;
    }
    st->my_mon_id = my_mon_id;
    st->cap = cap;
    st->capid = capid;
    st->give_away = false;

    err = capops_copy(cap, conn->core_id, cap_send_copy_cont, st);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "starting copy for cap_send_request failed");
    }
}

static void
cap_move_cnode_delete_cont(errval_t status, void *gen_st)
{
    struct send_cap_st *st = (struct send_cap_st*)gen_st;
    if (err_is_fail(status)) {
        DEBUG_ERR(status, "deleting cnode cap for move, will leak");
    }

    struct remote_conn_state *conn = remote_conn_lookup(st->my_mon_id);

    status = capops_copy(st->cap, conn->core_id, cap_send_copy_cont, st);
    if (err_is_fail(status)) {
        DEBUG_ERR(status, "starting copy for cap_send_request failed");
    }
}

static void
cap_move_request(struct monitor_binding *b, uintptr_t my_mon_id,
                 struct capref croot, capaddr_t caddr, uint8_t bits,
                 uint32_t capid)
{
    errval_t err;
    debug_printf("cap_move_request\n");

    // TODO: check for same-core tx?

    struct send_cap_st *st;
    st = malloc(sizeof(*st));
    if (!st) {
        err = LIB_ERR_MALLOC_FAIL;
    }
    st->my_mon_id = my_mon_id;
    st->capid = capid;
    st->give_away = true;

    // XXX: An ugly way to copy the cap out of the caller's CNode

    struct capability capdata = { .type = 0 };
    err = monitor_domains_cap_identify(croot, caddr, bits, &capdata);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "identifying cap for cap_move_request (%08"PRIxCADDR"/"PRIu8")", caddr, bits);
        st->cap = NULL_CAP;
        goto do_send;
    }

    err = slot_alloc(&st->cap);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "allocating slot for cap_move_request failed");
        st->cap = NULL_CAP;
        goto do_send;
    }

    err = monitor_copy_if_exists(&capdata, st->cap);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "copying cap");
        slot_free(st->cap);
        st->cap = NULL_CAP;
        goto do_send;
    }

    err = invoke_cnode_delete(croot, caddr, bits);
    if (err_is_fail(err) && err_no(err) != SYS_ERR_CAP_NOT_FOUND) {
        DEBUG_ERR(err, "deleting source copy, will leak");
    }

do_send:
    err = capops_delete(get_cap_domref(croot), cap_move_cnode_delete_cont, st);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "starting copy for cap_send_request failed");
    }
}

#if 0
struct capref domains[MAX_DOMAINS];

static void assign_domain_id_request(struct monitor_binding *b, uintptr_t ust,
                                     struct capref disp, struct capref ep)
{
    for(domainid_t id = 1; id < MAX_DOMAINS; id++) {
        if(domains[id].cnode.address_bits == 0) {
            domains[id] = ep;
            errval_t err = invoke_domain_id(disp, id);
            assert(err_is_ok(err));

            err = b->tx_vtbl.assign_domain_id_reply(b, NOP_CONT, ust, id);
            if (err_is_fail(err)) {
                USER_PANIC_ERR(err, "assign domain ID failed\n");
            }
            return;
        }
    }

    // Return error
    errval_t err = b->tx_vtbl.assign_domain_id_reply(b, NOP_CONT, ust, 0);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "assign domain ID failed\n");
    }
}
#endif

static void span_domain_request(struct monitor_binding *mb,
                                uintptr_t domain_id, uint8_t core_id,
                                struct capref vroot, struct capref disp)
{
    errval_t err, err2;

    trace_event(TRACE_SUBSYS_MONITOR, TRACE_EVENT_SPAN0, core_id);
    
    struct span_state *state;
    uintptr_t state_id;

    err = span_state_alloc(&state, &state_id);
    if (err_is_fail(err)) {
        err_push(err, MON_ERR_SPAN_STATE_ALLOC);
        goto reply;
    }

    state->core_id   = core_id;
    state->vroot     = vroot;
    state->mb        = mb;
    state->domain_id = domain_id;

    trace_event(TRACE_SUBSYS_MONITOR, TRACE_EVENT_SPAN1, core_id);

    /* Look up the destination monitor */
    struct intermon_binding *ib;
    err = intermon_binding_get(core_id, &ib);
    if (err_is_fail(err)) {
        goto reply;
    }

    /* Idenfity vroot */
    struct capability vroot_cap;
    err = monitor_cap_identify(vroot, &vroot_cap);
    if (err_is_fail(err)) {
        err_push(err, MON_ERR_CAP_IDENTIFY);
        goto reply;
    }
    if (vroot_cap.type != ObjType_VNode_x86_64_pml4) { /* Check type */
        err = MON_ERR_WRONG_CAP_TYPE;
        goto reply;
    }

    /* Identify the dispatcher frame */
    struct frame_identity frameid;
    err = invoke_frame_identify(disp, &frameid);
    if (err_is_fail(err)) {
        err_push(err, LIB_ERR_FRAME_IDENTIFY);
        goto reply;
    }

    bool has_descendants;
    err = monitor_cap_remote(disp, true, &has_descendants);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "monitor_cap_remote failed");
        return;
    }
    err = monitor_cap_remote(vroot, true, &has_descendants);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "monitor_cap_remote failed");
        return;
    }

    /* Send msg to destination monitor */
    err = ib->tx_vtbl.span_domain_request(ib, NOP_CONT, state_id,
                                          vroot_cap.u.vnode_x86_64_pml4.base,
                                          frameid.base, frameid.bits);

    if (err_is_fail(err)) {
        err_push(err, MON_ERR_SEND_REMOTE_MSG);
        goto reply;
    }
    goto cleanup;

 reply:
    err2 = mb->tx_vtbl.span_domain_reply(mb, NOP_CONT, err, domain_id);
    if (err_is_fail(err2)) {
        // XXX: Cleanup?
        USER_PANIC_ERR(err2, "Failed to reply to the user domain");
    }
    if(state_id != 0) {
        err2 = span_state_free(state_id);
        if (err_is_fail(err2)) {
            USER_PANIC_ERR(err2, "Failed to free span state");
        }
    }

 cleanup:
    err2 = cap_destroy(vroot);
    if (err_is_fail(err2)) {
        USER_PANIC_ERR(err2, "Failed to destroy span_vroot cap");
    }
    err2 = cap_destroy(disp);
    if (err_is_fail(err2)) {
        USER_PANIC_ERR(err2, "Failed to destroy disp cap");
    }
}

static void num_cores_request(struct monitor_binding *b)
{
    /* XXX: This is deprecated and shouldn't be used: there's nothing useful you
     * can do with the result, unless you assume that core IDs are contiguous
     * and start from zero, which is a false assumption! Go ask the SKB...
     */

    debug_printf("Application invoked deprecated num_cores_request() API."
                 " Please fix it!\n");

    /* Send reply */
    errval_t err = b->tx_vtbl.num_cores_reply(b, NOP_CONT, num_monitors);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "sending num_cores_reply failed");
    }
}

struct monitor_rx_vtbl the_table = {
    .alloc_iref_request = alloc_iref_request,

    .bind_lmp_client_request= bind_lmp_client_request,
    .bind_lmp_reply_monitor = bind_lmp_reply,

    .boot_core_request = boot_core_request,
    .boot_initialize_request = boot_initialize_request,
    .multiboot_cap_request = ms_multiboot_cap_request,

    .new_monitor_binding_request = new_monitor_binding_request,

    .get_mem_iref_request  = get_mem_iref_request,
    .get_name_iref_request = get_name_iref_request,
    .get_ramfs_iref_request = get_ramfs_iref_request,
    .set_mem_iref_request  = set_mem_iref_request,
    .set_name_iref_request = set_name_iref_request,
    .set_ramfs_iref_request = set_ramfs_iref_request,
    .get_monitor_rpc_iref_request  = get_monitor_rpc_iref_request,

    .cap_send_request = cap_send_request,
    .cap_move_request = cap_move_request,

    .span_domain_request    = span_domain_request,

    .num_cores_request  = num_cores_request,

    //.assign_domain_id_request = assign_domain_id_request,
};

errval_t monitor_client_setup(struct spawninfo *si)
{
    errval_t err;

    struct monitor_lmp_binding *b =
        malloc(sizeof(struct monitor_lmp_binding));
    assert(b != NULL);

    // setup our end of the binding
    err = monitor_client_lmp_accept(b, get_default_waitset(),
                                    DEFAULT_LMP_BUF_WORDS);
    if (err_is_fail(err)) {
        free(b);
        return err_push(err, LIB_ERR_MONITOR_CLIENT_ACCEPT);
    }

    // copy the endpoint cap to the recipient
    struct capref dest = {
        .cnode = si->rootcn,
        .slot  = ROOTCN_SLOT_MONITOREP,
    };

    err = cap_copy(dest, b->chan.local_cap);
    if (err_is_fail(err)) {
        // TODO: destroy binding
        return err_push(err, LIB_ERR_CAP_COPY);
    }

    // Copy the performance monitoring cap to all spawned processes.
    struct capref src;
    dest.cnode = si->taskcn;
    dest.slot = TASKCN_SLOT_PERF_MON;
    src.cnode = cnode_task;
    src.slot = TASKCN_SLOT_PERF_MON;
    err = cap_copy(dest, src);
    if (err_is_fail(err)) {
        return err_push(err, INIT_ERR_COPY_PERF_MON);
    }    

    // copy our receive vtable to the binding
    monitor_server_init(&b->b);

    return SYS_ERR_OK;
}

errval_t monitor_client_setup_mem_serv(void)
{
    /* construct special-case LMP connection to mem_serv */
    static struct monitor_lmp_binding mcb;
    struct waitset *ws = get_default_waitset();
    errval_t err;

    err = monitor_client_lmp_accept(&mcb, ws, DEFAULT_LMP_BUF_WORDS);
    if(err_is_fail(err)) {
        USER_PANIC_ERR(err, "monitor_client_setup_mem_serv");
    }
    assert(err_is_ok(err));

    /* Send the cap for this endpoint to init, who will pass it to the monitor */
    err = lmp_ep_send0(cap_initep, 0, mcb.chan.local_cap);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "lmp_ep_send0 failed");
    }

    // copy our receive vtable to the binding
    monitor_server_init(&mcb.b);

    // XXX: handle messages (ie. block) until the monitor binding is ready
    while (capref_is_null(mcb.chan.remote_cap)) {
        err = event_dispatch(ws);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "in event_dispatch waiting for mem_serv binding");
            return err_push(err, LIB_ERR_EVENT_DISPATCH);
        }
    }

    return SYS_ERR_OK;
}

/// Setup a dummy monitor binding that "sends" all requests to the local handlers
errval_t monitor_client_setup_monitor(void)
{
    monitor_loopback_init(&monitor_self_binding);
    monitor_server_init(&monitor_self_binding);
    set_monitor_binding(&monitor_self_binding);
    idc_init();
    // XXX: Need a waitset here or loopback won't work as expected
    // when binding to the ram_alloc service
    monitor_self_binding.mutex.equeue.waitset = get_default_waitset();

    return SYS_ERR_OK;
}

errval_t monitor_server_init(struct monitor_binding *b)
{
    struct monitor_state *lst = malloc(sizeof(struct monitor_state));
    assert(lst != NULL);
    lst->queue.head = lst->queue.tail = NULL;

    // copy our receive vtable to the new binding
    b->rx_vtbl = the_table;
    b->st = lst;
    // TODO: set error_handler

#ifdef CONFIG_INTERCONNECT_DRIVER_UMP
    errval_t err;
    err = ump_monitor_init(b);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "ump_monitor_init failed");
    }
#endif

#ifdef CONFIG_INTERCONNECT_DRIVER_MULTIHOP
    errval_t err2;
    err2 = multihop_monitor_init(b);
    if (err_is_fail(err2)) {
        USER_PANIC_ERR(err2, "multihop_monitor_init failed");
    }
#endif // CONFIG_INTERCONNECT_DRIVER_MULTIHOP

    return monitor_server_arch_init(b);
}
