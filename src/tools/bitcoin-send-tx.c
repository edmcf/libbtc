/*

 The MIT License (MIT)

 Copyright (c) 2017 Jonas Schnelli

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
 OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 OTHER DEALINGS IN THE SOFTWARE.
 
*/

#include "libbtc-config.h"

#include <btc/chainparams.h>
#include <btc/ecc.h>
#include <btc/net.h>
#include <btc/protocol.h>
#include <btc/serialize.h>
#include <btc/tool.h>
#include <btc/tx.h>
#include <btc/utils.h>

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

static struct option long_options[] =
{
    {"testnet", no_argument, NULL, 't'},
    {"regtest", no_argument, NULL, 'r'},
    {"ips", no_argument, NULL, 'i'},
    {"debug", no_argument, NULL, 'd'},
    {"timeout", no_argument, NULL, 's'},
    {NULL, 0, NULL, 0}
};

static void print_version() {
    printf("Version: %s %s\n", PACKAGE_NAME, PACKAGE_VERSION);
}

static void print_usage() {
    print_version();
    printf("Usage: bitcoin-send-tx (-i|-ips <ip,ip,...]>) (-t[--testnet]) (-r[--regtest]) (-d[--debug]) (-s[--timeout] <secs>) <txhex>\n");
    printf("\nExamples: \n");
    printf("Send a TX to random peers on testnet:\n");
    printf("> bitcoin-send-tx --testnet <txhex>\n\n");
    printf("Send a TX to specific peers on mainnet:\n");
    printf("> bitcoin-send-tx -i 127.0.0.1:8333,192.168.0.1:8333 <txhex>\n\n");
}

static bool showError(const char *er)
{
    printf("Error: %s\n", er);
    return 1;
}

btc_bool broadcast_tx(const btc_chainparams* chain, const btc_tx *tx, const char *ips, int timeout, btc_bool debug);

int main(int argc, char *argv[])
{
    int ret = 0;
    int long_index =0;
    char opt = 0;
    char *data = 0;
    char *ips = 0;
    int debug = 0;
    int timeout = 10;
    const btc_chainparams* chain = &btc_chainparams_main;

    if (argc <= 1 || strlen(argv[argc-1]) == 0 || argv[argc-1][0] == '-')
    {
        /* exit if no command was provided */
        print_usage();
        exit(EXIT_FAILURE);
    }
    data = argv[argc-1];

    /* get arguments */
    while ((opt = getopt_long_only(argc, argv,"i:trds:", long_options, &long_index )) != -1) {
        switch (opt) {
            case 't' :
                chain = &btc_chainparams_test;
                break;
            case 'r' :
                chain = &btc_chainparams_regtest;
                break;
            case 'd' :
                debug = 1;
                break;
            case 's' :
                timeout = (int)strtol(optarg, (char **)NULL, 10);
                break;
            case 'i' : ips = optarg;
                break;
            case 'v' :
                print_version();
                exit(EXIT_SUCCESS);
                break;
            default: print_usage();
                exit(EXIT_FAILURE);
        }
    }

    if (data == NULL || strlen(data) == 0 || strlen(data) > BTC_MAX_P2P_MSG_SIZE) {
        return showError("Transaction in invalid or to large.\n");
    }
    uint8_t *data_bin = btc_malloc(strlen(data)/2+1);
    int outlen = 0;
    utils_hex_to_bin(data, data_bin, strlen(data), &outlen);

    btc_tx* tx = btc_tx_new();
    if (btc_tx_deserialize(data_bin, outlen, tx, NULL)) {
        broadcast_tx(chain, tx, ips, timeout, debug);
    }
    else {
        showError("Transaction is invalid\n");
        ret = 1;
    }
    btc_free(data_bin);
    btc_tx_free(tx);

    return ret;
}

struct broadcast_ctx {
    const btc_tx *tx;
    int timeout;
    int debuglevel;
    int connected_to_peers;
    int max_peers_to_connect;
    int max_peers_to_inv;
    int inved_to_peers;
    int getdata_from_peers;
    int found_on_non_inved_peers;
};

static btc_bool broadcast_timer_cb(btc_node *node, uint64_t *now)
{
    struct broadcast_ctx *ctx = (struct broadcast_ctx  *)node->nodegroup->ctx;

    if (node->time_started_con > 0) {
        node->nodegroup->log_write_cb("timer node %d, delta: %" PRIu64 " secs\n", node->nodeid, (*now - node->time_started_con));
    }
    if (node->time_started_con + ctx->timeout < *now)
        btc_node_disconnect(node);

    if ((node->hints & (1 << 1)) == (1 << 1))
    {
        btc_node_disconnect(node);
    }

    if ((node->hints & (1 << 2)) == (1 << 2))
    {
        btc_node_disconnect(node);
    }

    /* return true = run internal timer logic (ping, disconnect-timeout, etc.) */
    return true;
}

void broadcast_handshake_done(struct btc_node_ *node)
{
    printf("Successfully connected to peer %d\n", node->nodeid);
    struct broadcast_ctx *ctx = (struct broadcast_ctx  *)node->nodegroup->ctx;
    ctx->connected_to_peers++;

    if (ctx->inved_to_peers >= ctx->max_peers_to_inv) {
        return;
    }

    /* create a INV */
    cstring *inv_msg_cstr = cstr_new_sz(256);
    btc_p2p_inv_msg inv_msg;
    memset(&inv_msg, 0, sizeof(inv_msg));

    uint8_t hash[32];
    btc_tx_hash(ctx->tx, hash);
    btc_p2p_msg_inv_init(&inv_msg, BTC_INV_TYPE_TX, hash);

    /* serialize the inv count (1) */
    ser_varlen(inv_msg_cstr, 1);
    btc_p2p_msg_inv_ser(&inv_msg, inv_msg_cstr);

    cstring *p2p_msg = btc_p2p_message_new(node->nodegroup->chainparams->netmagic, BTC_MSG_INV, inv_msg_cstr->str, inv_msg_cstr->len);
    cstr_free(inv_msg_cstr, true);
    btc_node_send(node, p2p_msg);
    cstr_free(p2p_msg, true);

    /* set hint bit 0 == inv sent */
    node->hints |= (1 << 0);
    ctx->inved_to_peers++;
}

void broadcast_post_cmd(struct btc_node_ *node, btc_p2p_msg_hdr *hdr, struct const_buffer *buf) {
    struct broadcast_ctx *ctx = (struct broadcast_ctx  *)node->nodegroup->ctx;
    if (strcmp(hdr->command, BTC_MSG_INV) == 0)
    {
        /* hash the tx */
        /* TODO: cache the hash */
        uint8_t hash[32];
        btc_tx_hash(ctx->tx, hash);

        //  decompose
        uint32_t vsize;
        if (!deser_varlen(&vsize, buf)) { btc_node_missbehave(node); return; };
        for (unsigned int i=0;i<vsize;i++)
        {
            btc_p2p_inv_msg inv_msg;
            if (!btc_p2p_msg_inv_deser(&inv_msg, buf)) { btc_node_missbehave(node); return; }
            if (memcmp(hash, inv_msg.hash, 32) == 0) {
                // txfound
                /* set hint bit 2 == tx found on peer*/
                node->hints |= (1 << 2);
                printf("node %d has the tx\n", node->nodeid);
                ctx->found_on_non_inved_peers++;
                printf("tx successfully seen on node %d\n", node->nodeid);
            }
        }
    }
    else if (strcmp(hdr->command, BTC_MSG_GETDATA) == 0 && ((node->hints & (1 << 1)) != (1 << 1)))
    {
        ctx->getdata_from_peers++;
        //only allow a single object in getdata for the broadcaster
        uint32_t vsize;
        if (!deser_varlen(&vsize, buf) || vsize!=1) { btc_node_missbehave(node); return; }

        btc_p2p_inv_msg inv_msg;
        memset(&inv_msg, 0, sizeof(inv_msg));
        if (!btc_p2p_msg_inv_deser(&inv_msg, buf) || inv_msg.type != BTC_INV_TYPE_TX) { btc_node_missbehave(node); return; };

        /* send the tx */
        cstring* tx_ser = cstr_new_sz(1024);
        btc_tx_serialize(tx_ser, ctx->tx);
        cstring *p2p_msg = btc_p2p_message_new(node->nodegroup->chainparams->netmagic, BTC_MSG_TX, tx_ser->str, tx_ser->len);
        cstr_free(tx_ser, true);
        btc_node_send(node, p2p_msg);
        cstr_free(p2p_msg, true);

        /* set hint bit 1 == tx sent */
        node->hints |= (1 << 1);

        printf("tx successfully sent to node %d\n", node->nodeid);
    }
}

btc_bool broadcast_tx(const btc_chainparams* chain, const btc_tx *tx, const char *ips, int timeout, btc_bool debug)
{
    struct broadcast_ctx ctx;
    ctx.tx = tx;
    ctx.debuglevel = debug;
    ctx.timeout = timeout;
    ctx.max_peers_to_inv = 2;
    ctx.found_on_non_inved_peers = 0;
    ctx.getdata_from_peers = 0;
    ctx.inved_to_peers = 0;
    ctx.connected_to_peers = 0;
    ctx.max_peers_to_connect = 6;

    /* create a node group */
    btc_node_group* group = btc_node_group_new(chain);
    group->desired_amount_connected_nodes = ctx.max_peers_to_connect;
    group->ctx = &ctx;

    /* set the timeout callback */
    group->periodic_timer_cb = broadcast_timer_cb;

    /* set a individual log print function */
    if (debug) {
        group->log_write_cb = net_write_log_printf;
    }
    group->postcmd_cb = broadcast_post_cmd;
    group->handshake_done_cb = broadcast_handshake_done;

    if (ips == NULL) {
        /* === DNS QUERY === */
        /* get a couple of peers from a seed */
        vector *ips_dns = vector_new(10, free);
        const btc_dns_seed seed = chain->dnsseeds[0];
        if (strlen(seed.domain) == 0)
        {
            return -1;
        }
        /* todo: make sure we have enought peers, eventually */
        /* call another seeder */
        btc_get_peers_from_dns(seed.domain, ips_dns, chain->default_port, AF_INET);
        for (unsigned int i = 0; i<ips_dns->len; i++)
        {
            char *ip = (char *)vector_idx(ips_dns, i);

            /* create a node */
            btc_node *node = btc_node_new();
            if (btc_node_set_ipport(node, ip) > 0) {
                /* add the node to the group */
                btc_node_group_add_node(group, node);
            }
        }
        vector_free(ips_dns, true);
    }
    else {
        // add comma seperated ips (nodes)
        char working_str[64];
        memset(working_str, 0, sizeof(working_str));
        size_t offset = 0;
        for(unsigned int i=0;i<=strlen(ips);i++)
        {
            if (i == strlen(ips) || ips[i] == ',') {
                btc_node *node = btc_node_new();
                if (btc_node_set_ipport(node, working_str) > 0) {
                    btc_node_group_add_node(group, node);
                }
                offset = 0;
                memset(working_str, 0, sizeof(working_str));
            }
            else if (ips[i] != ' ' && offset < sizeof(working_str)) {
                working_str[offset]=ips[i];
                offset++;
            }
        }
    }


    /* connect to the next node */
    printf("Start broadcasting process with timeout of %d seconds\n", timeout);
    printf("Trying to connect to nodes...\n");
    btc_node_group_connect_next_nodes(group);

    /* start the event loop */
    btc_node_group_event_loop(group);

    /* cleanup */
    btc_node_group_free(group); //will also free the nodes structures from the heap

    printf("\n\nResult:\n=============\n");
    printf("Max nodes to connect to: %d\n", ctx.max_peers_to_connect);
    printf("Connected to nodes: %d\n", ctx.connected_to_peers);
    printf("Informed nodes: %d\n", ctx.inved_to_peers);
    printf("Requested from nodes: %d\n", ctx.getdata_from_peers);
    printf("Seen on other nodes: %d\n", ctx.found_on_non_inved_peers);
    return 1;
}