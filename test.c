#include "stdio.h"
#include <string.h>

#include "ccp.h"

#ifdef __GNUC__
    #define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
    #define UNUSED(x) x
#endif

//===========================================
// Mock Datapath
//===========================================

u32 now_us; // emulated time in microseconds

#define  BIGGEST_MSG_SIZE  1024
char expected_sent_msg[BIGGEST_MSG_SIZE];
int expecting_send;

struct test_conn {
    u32 curr_cwnd;
    u32 curr_rate;
};

static void test_ccp_set_cwnd(struct ccp_datapath *UNUSED(dp), struct ccp_connection *conn, u32 cwnd) {
    struct test_conn *c = (struct test_conn*) ccp_get_impl(conn);
    c->curr_cwnd = cwnd;
}

static void test_ccp_set_rate(struct ccp_datapath *UNUSED(dp), struct ccp_connection *conn, u32 rate) {
    struct test_conn *c = (struct test_conn*) ccp_get_impl(conn);
    c->curr_rate = rate;
}

static void test_ccp_set_rate_rel(struct ccp_datapath *UNUSED(dp), struct ccp_connection *UNUSED(conn), u32 UNUSED(cwnd)) {
    return;
}

static int test_ccp_send_msg(struct ccp_datapath *UNUSED(dp), struct ccp_connection *UNUSED(conn), char *msg, int msg_size) {
    if (expecting_send <= 0) {
        printf("Not expecting send");
        goto fail;
    }
    
    if (msg_size != expecting_send) {
        printf("Wrong size: expected %d got %d", expecting_send, msg_size);
        goto fail;
    }

    if (memcmp(&expected_sent_msg, msg, msg_size) != 0) {
        printf("Wrong msg");
        goto fail;
    }

    expecting_send = 0;
    memset(&expected_sent_msg, 0, BIGGEST_MSG_SIZE);

    return 0;

fail:
    printf(": got: [");
    for (int i = 0; i < msg_size; i++) {
        printf("%02x, ", msg[i]);
    }
    printf("] ");
    if (expecting_send > 0) {
        printf("\n Expected: [");
        for (int i = 0; i < expecting_send; i++) {
            printf("%02x, ", expected_sent_msg[i]);
        }
        printf("] ");
    }
    return -1;
}

static u64 test_ccp_time_now(void) {
    return now_us;
}

static u64 test_ccp_since_usecs(u64 then) {
    return then - now_us;
}

static u64 test_ccp_after_usecs(u64 usecs) {
    return now_us + usecs;
}

//===========================================
// Test Initialization and Helpers
//===========================================

int test_init(struct ccp_connection **conn, struct test_conn *my_conn, struct ccp_datapath *dp) {
    int ok;
    printf("initializing libccp...");
    ok = ccp_init(dp);
    if (ok < 0) {
        printf("ccp_init error: %d\n", ok);
        return -1;
    }

    // a fake flow arrives!
    struct ccp_datapath_info info = {
        .init_cwnd = 100,
        .mss = 10,
        .src_ip = 1,
        .src_port = 2,
        .dst_ip = 3, 
        .dst_port = 4,
        .congAlg = "test",
    };

    char create_msg[32] = {
        0x00,0x10,0x01,0x00,
        0x00,0x00,0x64,0x00,
        0x00,0x00,0x0a,0x00,
        0x00,0x00,0x01,0x00,
        0x00,0x00,0x02,0x00,
        0x00,0x00,0x03,0x00,
        0x00,0x00,0x04,0x00,
        0x00,0x00,0x00,0x00,
    };

    memcpy(&expected_sent_msg, &create_msg, 32);
    expecting_send = 32;
    *conn = ccp_connection_start((void*) &my_conn, &info);
    if (expecting_send != 0) {
        printf("err: did not send\n");
        return -2;
    } else {
        printf("ok\n");
        return 0;
    }
}

int install_helper(char *dp, size_t dp_len) {
    int ok = ccp_read_msg(dp, dp_len);
    if (ok < 0) {
        printf("read install message error: %d", ok);
        return -1;
    }

    return 0;
}

int getreport_helper(char *expected, size_t msg_len, struct ccp_connection *conn) {
    memcpy(&expected_sent_msg, expected, msg_len);
    expecting_send = msg_len;
    int ok = ccp_invoke(conn);
    if (ok < 0) {
        printf("ccp_invoke error: %d", ok);
        return -1;
    }

    if (expecting_send != 0) {
        printf("did not send");
        memset(&expected_sent_msg, 0, BIGGEST_MSG_SIZE);
        expecting_send = 0;
        return -2;
    }
    
    return 0;
}

// ------------------------------------------
// Tests
// ------------------------------------------

int test_basic(struct ccp_connection *conn) {
    int ok;
    char dp[104] = {
        2,                                              // INSTALL
        0x34,                                           // length = 0x34 * 2 = 52 *2 = 104
        1, 0, 0, 0,                                     // sock_id = 1
        1, 0, 0, 0,                                     // num_events = 1
        5, 0, 0, 0,                                     // num_instrs = 5
        1, 1, 2, 3,                                     // event { flag-idx=1, num-flag=1, body-idx=2, num-body=3 }
        2, 5, 0, 0, 0, 0, 5, 0, 0, 0, 0, 1, 0, 0, 0, 0, // (def (Report.foo 0))
        1, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 1, 0, 0, 0, // (when true
        0, 6, 0, 0, 0, 0, 5, 0, 0, 0, 0, 1, 1, 0, 0, 0, //      ----------------(+ Report.foo 1)
        1, 5, 0, 0, 0, 0, 5, 0, 0, 0, 0, 6, 0, 0, 0, 0, //     (bind Report.foo ^^^^^^^^^^^^^^^^)
        1, 2, 2, 0, 0, 0, 2, 2, 0, 0, 0, 1, 1, 0, 0, 0, //     (bind __shouldReport true)
    };
    char report_msg[18] = {
        0x01,
        0x09,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    printf("%s...", __func__);
    fflush(stdout);

    ok = install_helper(dp, 104);
    if (ok < 0) {
        printf("\n");
        return -1;
    }

    ok = getreport_helper(report_msg, 18, conn);
    if (ok < 0) {
        printf("\n");
        return -1;
    }

    printf("ok\n");
    return 0;
}

int test_primitives(struct ccp_connection *conn) {
    int ok;
    char dp[82] = {
        2,                                                       // INSTALL
        0x29,                                                    // length = 0x29 * 2 = 41 *2 = 82
        1, 0, 0, 0,                                              // sock_id = 1
        1, 0, 0, 0,                                              // num_events = 1
        4, 0, 0, 0,                                              // num_instrs = 2
        1, 1, 2, 2,                                              // event { flag-idx=1, num-flag=1, body-idx=2, num-body=2 }
        2, 5, 0, 0, 0, 0, 5, 0,  0, 0, 0, 1, 255, 255, 255, 255, // (def (Report.minrtt +infinity))
        8, 2, 0, 0, 0, 0, 4, 13, 0, 0, 0, 5, 0,   0,   0,   0,   // (when (< Flow.rtt_sample_us Report.minrtt)
        1, 5, 0, 0, 0, 0, 5, 0,  0, 0, 0, 4, 13,  0,   0,   0,   //     (bind Report.minrtt Flow.rtt_sample_us) 
        1, 2, 2, 0, 0, 0, 2, 2,  0, 0, 0, 1, 1,   0,   0,   0,   //     (bind __shouldReport true)
    };
    char report_msg[18] = {
        0x01,
        0x09,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0xed, 0xfe, 0xfe, 0xca, 0x00, 0x00, 0x00, 0x00,
    };
    printf("%s...", __func__);
    fflush(stdout);

    conn->prims.rtt_sample_us = 0xcafefeed;

    ok = install_helper(dp, 104);
    if (ok < 0) {
        printf("\n");
        return -1;
    }

    ok = getreport_helper(report_msg, 18, conn);
    if (ok < 0) {
        printf("\n");
        return -1;
    }

    printf("ok\n");
    return 0;
}

int test_multievent(struct ccp_connection *conn) {
    int ok;
    char dp[162] = {
        2,                                                       // INSTALL
        0x51,                                                    // length = 0x51 * 2 = 81 *2 = 162
        1, 0, 0, 0,                                              // sock_id = 1
        2, 0, 0, 0,                                              // num_events = 2
        8, 0, 0, 0,                                              // num_instrs = 8
        2, 1, 3, 2,                                              // event { flag-idx=2, num-flag=1, body-idx=3, num-body=2 }
        5, 1, 6, 2,                                              // event { flag-idx=5, num-flag=1, body-idx=6, num-body=2 }
        2, 5, 0, 0, 0, 0, 5, 0,  0, 0, 0, 1, 0,   0,   0,   0,   // -----(Control.minrtt +infinity)--------------------
        2, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 1, 255, 255, 255, 255, // (def ^^^^^^^^^^^^^^^^^^^^^^^^^^ (Report.counter 0))
        8, 2, 0, 0, 0, 0, 4, 13, 0, 0, 0, 0, 0,   0,   0,   0,   // (when (< Flow.rtt_sample_us Control.minrtt)
        1, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 4, 13,  0,   0,   0,   //     (bind Control.minrtt Flow.rtt_sample_us) 
        1, 2, 2, 0, 0, 0, 2, 2,  0, 0, 0, 1, 1,   0,   0,   0,   //     (bind __shouldReport true))
        1, 2, 0, 0, 0, 0, 2, 0,  0, 0, 0, 1, 1,   0,   0,   0,   // (when true
        0, 6, 0, 0, 0, 0, 5, 0,  0, 0, 0, 1, 1,   0,   0,   0,   //     ---------------------(+ Report.counter 1)-
        1, 5, 0, 0, 0, 0, 5, 0,  0, 0, 0, 6, 0,   0,   0,   0    //     (bind Report.counter ^^^^^^^^^^^^^^^^^^^^))
    };
    char first_report_msg[18] = {
        0x01,
        0x09,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    char report_msg[18] = {
        0x01,
        0x09,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    printf("%s...", __func__);

    conn->prims.rtt_sample_us = 0xcafefeed;
    fflush(stdout);
    ok = install_helper(dp, 162);
    if (ok < 0) {
        printf("install\n");
        return -1;
    }
    
    ok = getreport_helper(first_report_msg, 18, conn);
    if (ok < 0) {
        printf("failed on first report\n");
        return -1;
    }

    ok = ccp_invoke(conn);
    if (ok < 0) {
        printf("ccp_invoke 1 error: %d", ok);
        return -1;
    }
    
    ok = ccp_invoke(conn);
    if (ok < 0) {
        printf("ccp_invoke 2 error: %d", ok);
        return -1;
    }

    conn->prims.rtt_sample_us = 0xcafefeeb;
    ok = getreport_helper(report_msg, 18, conn);
    if (ok < 0) {
        printf("failed on second report\n");
        return -1;
    }

    printf("ok\n");
    return 0;
}

int test_fallthrough(struct ccp_connection *conn) {
    int ok;
    char dp[170] = {
        2,                                                       // INSTALL
        0x56,                                                    // length = 0x56 * 2 = 86 *2 = 172
        1, 0, 0, 0,                                              // sock_id = 1
        2, 0, 0, 0,                                              // num_events = 2
        9, 0, 0, 0,                                              // num_instrs = 9
        2, 1, 3, 3,                                              // event { flag-idx=2, num-flag=1, body-idx=3, num-body=3 }
        6, 1, 7, 2,                                              // event { flag-idx=6, num-flag=1, body-idx=7, num-body=2 }
        2, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 1, 255, 255, 255, 255, // -----(Control.minrtt +infinity)--------------------
        2, 5, 0, 0, 0, 0, 5, 0,  0, 0, 0, 1, 0,   0,   0,   0,   // (def ^^^^^^^^^^^^^^^^^^^^^^^^^^ (Report.counter 0))
        8, 2, 0, 0, 0, 0, 4, 13, 0, 0, 0, 0, 0,   0,   0,   0,   // (when (< Flow.rtt_sample_us Control.minrtt)
        1, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 4, 13,  0,   0,   0,   //     (bind Control.minrtt Flow.rtt_sample_us) 
        1, 2, 2, 0, 0, 0, 2, 2,  0, 0, 0, 1, 1,   0,   0,   0,   //     (bind __shouldReport true)
        1, 2, 1, 0, 0, 0, 2, 1,  0, 0, 0, 1, 1,   0,   0,   0,   //     (bind __shouldContinue true))
        1, 2, 0, 0, 0, 0, 2, 0,  0, 0, 0, 1, 1,   0,   0,   0,   // (when true
        0, 6, 0, 0, 0, 0, 5, 0,  0, 0, 0, 1, 1,   0,   0,   0,   //     ---------------------(+ Report.counter 1)-
        1, 5, 0, 0, 0, 0, 5, 0,  0, 0, 0, 6, 0,   0,   0,   0,   //     (bind Report.counter ^^^^^^^^^^^^^^^^^^^^))
    };
    char first_report_msg[18] = {
        0x01,
        0x09,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    char report_msg[18] = {
        0x01,
        0x09,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    printf("%s...", __func__);

    conn->prims.rtt_sample_us = 0xcafefeed;
    fflush(stdout);
    ok = install_helper(dp, 172);
    if (ok < 0) {
        printf("install\n");
        return -1;
    }
    
    ok = getreport_helper(first_report_msg, 18, conn);
    if (ok < 0) {
        printf("failed on first report\n");
        return -1;
    }

    ok = ccp_invoke(conn);
    if (ok < 0) {
        printf("ccp_invoke 1 error: %d", ok);
        return -1;
    }
    
    ok = ccp_invoke(conn);
    if (ok < 0) {
        printf("ccp_invoke 2 error: %d", ok);
        return -1;
    }

    conn->prims.rtt_sample_us = 0xcafefeeb;
    ok = getreport_helper(report_msg, 18, conn);
    if (ok < 0) {
        printf("failed on second report\n");
        return -1;
    }

    printf("ok\n");
    return 0;
}

int test_read_implicit(struct ccp_connection *conn) {
    int ok;
    char dp[114] = {
        2,                                              // INSTALL
        0x39,                                           // length = 0x39 * 2 = 57 * 2 = 114
        1, 0, 0, 0,                                     // sock_id = 1
        1, 0, 0, 0,                                     // num_events = 2
        6, 0, 0, 0,                                     // num_instrs = 6
        1, 1, 2, 4,                                     // event { flag-idx=2, num-flag=1, body-idx=3, num-body=2 }
        2, 5, 0, 0, 0, 0, 5, 0, 0, 0, 0, 1, 0, 0, 0, 0, // (def (Report.foo 0))
        1, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 1, 0, 0, 0, // (when true
        1, 2, 4, 0, 0, 0, 2, 4, 0, 0, 0, 1, 5, 0, 0, 0, //     (bind Cwnd 5)
        4, 6, 0, 0, 0, 0, 2, 4, 0, 0, 0, 1, 5, 0, 0, 0, //     ------------------(== Cwnd 5)-
        1, 5, 0, 0, 0, 0, 5, 0, 0, 0, 0, 6, 0, 0, 0, 0, //     (bind Control.foo ^^^^^^^^^^^) 
        1, 2, 2, 0, 0, 0, 2, 2, 0, 0, 0, 1, 1, 0, 0, 0  //     (bind __shouldReport true))
    };
    char report_msg[18] = {
        0x01,
        0x09,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    printf("%s...", __func__);
    fflush(stdout);

    ok = install_helper(dp, 114);
    if (ok < 0) {
        printf("install failed\n");
        return -1;
    }
    
    ok = getreport_helper(report_msg, 18, conn);
    if (ok < 0) {
        printf("failed on report\n");
        return -1;
    }

    printf("ok\n");
    return 0;
}

int main(int UNUSED(argc), char **UNUSED(argv)) {
    int ok = 0;
    now_us = 0;
    struct ccp_datapath dp = {
        .set_cwnd = test_ccp_set_cwnd,
        .set_rate_abs = test_ccp_set_rate,
        .set_rate_rel = test_ccp_set_rate_rel,
        .send_msg = test_ccp_send_msg,
        .now = test_ccp_time_now,
        .since_usecs = test_ccp_since_usecs,
        .after_usecs = test_ccp_after_usecs,
    };

    struct test_conn my_conn = {
        .curr_cwnd = 0,
        .curr_rate = 0,
    };

    struct ccp_connection *conn;

    ok = test_init(&conn, &my_conn, &dp);
    if (ok < 0) {
        goto ret;
    }

    ok = test_basic(conn);
    if (ok < 0) {
        goto ret;
    }

    ok = test_primitives(conn);
    if (ok < 0) {
        goto ret;
    }
    
    ok = test_multievent(conn);
    if (ok < 0) {
        goto ret;
    }
    
    ok = test_fallthrough(conn);
    if (ok < 0) {
        goto ret;
    }
    
    ok = test_read_implicit(conn);
    if (ok < 0) {
        goto ret;
    }

    char close_msg[10] = {
        0x01,
        0x05,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    memcpy(&expected_sent_msg, &close_msg, 10);
    expecting_send = 10;
    ccp_connection_free(conn->index);
  ret:
    ccp_free();
    return 0;
}
