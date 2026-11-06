/* End-to-end runs of the host stack against the simulated controller. */
#include "sim.h"
#include "../src/crc32.h"
#include "../src/host.h"
#include "../src/host_int.h"
#include "../src/log.h"
#include "../src/util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int sdp_find_ids(const unsigned char *p, int n, uint16_t *vid, uint16_t *pid);

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

typedef struct {
    int connects, disconnects, states, slot;
    pad_info info;
    pad_state last;
} seen;

static void on_connect(void *ud, int slot, const pad_info *info)
{
    seen *s = ud;
    s->connects++;
    s->slot = slot;
    s->info = *info;
}

static void on_state(void *ud, int slot, const pad_state *st)
{
    seen *s = ud;
    (void)slot;
    s->states++;
    s->last = *st;
}

static void on_disconnect(void *ud, int slot)
{
    seen *s = ud;
    (void)slot;
    s->disconnects++;
}

static const char *DB = "build/test_pads.db";

/* A DualShock 4 full report 0x11: cross and dpad-up held, left stick at
 * (0x20, 0xE0), L2 half pressed. */
static int ds4_report(unsigned char *r)
{
    memset(r, 0, 78);
    r[0] = 0x11;
    r[1] = 0xC0;
    r[3] = 0x20; r[4] = 0xE0; r[5] = 0x80; r[6] = 0x80;
    r[7] = 0x20 | 0x00;     /* cross, hat north */
    r[10] = 0x80;
    r[32] = 0x08;           /* battery 8/8, no cable */
    return 78;
}

static void run(host_t *h, int ms)
{
    long end = sim_now() + ms;
    while (sim_now() < end) host_poll(h, 5);
}

static host_t *open_host(sim_t *sim, seen *s)
{
    host_events ev = { on_connect, on_state, on_disconnect, s, NULL };
    return host_open(sim_hci(sim), DB, &ev);
}

static int test_pairing_seed(int drop_every, unsigned seed, int quiet);
static void test_pairing(int drop_every) { test_pairing_seed(drop_every, 0, 0); }

static int test_pairing_seed(int drop_every, unsigned seed, int quiet)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;
    unsigned char out[128];
    int n;

    int fail_before = g_fail;

    if (!quiet) printf("pairing a DualShock 4 (dropping every %d packets)\n", drop_every);
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    pad.drop_every = drop_every;
    pad.seed = seed;

    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) return 0;

    host_pair(h, 30);
    run(h, 15000);

    CHECK(s.connects == 1);
    CHECK(s.info.vid == 0x054C && s.info.pid == 0x05C4);
    CHECK(s.info.profile && strcmp(s.info.profile, "DualShock 4") == 0);
    CHECK(sim_sdp_queries(sim) >= 1);
    CHECK(host_paired_count(h) == 1);
    CHECK(sim_outputs(sim) >= 1);           /* switched it to report 0x11 */
    CHECK(s.states > 0);
    CHECK(s.last.buttons & 0x4000);         /* cross */
    CHECK(s.last.buttons & 0x10);           /* up */
    CHECK(s.last.lx == 0x20 && s.last.ly == 0xE0);
    CHECK(s.last.l2 == 0x80 && (s.last.buttons & 0x100));
    CHECK(s.last.has_battery && s.last.battery_pct == 100);

    /* Rumble and light bar reach the pad, with a CRC it accepts. */
    {
        pad_output o = { 200, 100, 1, 2, 3, 1, 0 };
        CHECK(host_set_output(h, s.slot, &o));
        n = sim_last_output(sim, out, sizeof out);
        CHECK(n == 79);
        CHECK(out[0] == 0xA2 && out[1] == 0x11);
        CHECK(out[7] == 100 && out[8] == 200);
        CHECK(out[9] == 1 && out[10] == 2 && out[11] == 3);
        CHECK(crc32_calc(out, 75) == ((uint32_t)out[75] | (uint32_t)out[76] << 8 |
                                      (uint32_t)out[77] << 16 | (uint32_t)out[78] << 24));
    }

    if (!drop_every && !quiet) {
        int before;

        printf("DS4 game feedback: preserve colours and bound failed stop attempts\n");
        CHECK(host_rumble_feedback(h, s.slot, 180, 90));
        n = sim_last_output(sim, out, sizeof out);
        CHECK(n == 79 && out[7] == 90 && out[8] == 180);
        CHECK(out[9] == 1 && out[10] == 2 && out[11] == 3);
        sim_fail_outputs(sim, 1);
        CHECK(!host_rumble_pulse(h, s.slot, 500));
        sim_fail_outputs(sim, 1);
        before = sim_output_attempts(sim);
        CHECK(!host_rumble_feedback(h, s.slot, 0, 0));
        run(h, 200);
        CHECK(sim_output_attempts(sim) == before + 2);
        CHECK(sim_last_output(sim, out, sizeof out) == 79);
        CHECK(out[7] == 0 && out[8] == 0);
        CHECK(host_rumble_feedback(h, s.slot, 180, 90));
        sim_fail_outputs(sim, 1);
        CHECK(!host_rumble_feedback(h, s.slot, 0, 0));
        CHECK(host_rumble_pulse(h, s.slot, 500));
        before = sim_output_attempts(sim);
        run(h, 200);
        CHECK(sim_output_attempts(sim) == before); /* Old stop retries cannot restart a test pulse. */
        run(h, 400);
        CHECK(host_rumble_feedback(h, s.slot, 180, 90));
        sim_fail_outputs(sim, 100);
        before = sim_output_attempts(sim);
        CHECK(!host_rumble_feedback(h, s.slot, 0, 0));
        run(h, 1000);
        CHECK(sim_output_attempts(sim) == before + 5);
        run(h, 1000);
        CHECK(sim_output_attempts(sim) == before + 5);
        sim_fail_outputs(sim, 0);
        CHECK(host_rumble_feedback(h, s.slot, 10, 10));
        CHECK(host_rumble_feedback(h, s.slot, 0, 0));
    }

    /* Input changes are followed. */
    pad.report[7] = 0x08 | 0x10;            /* square, hat centred */
    sim_set_report(sim, pad.report, pad.report_len);
    run(h, 200);
    CHECK((s.last.buttons & 0x8000) && !(s.last.buttons & 0x4000));
    CHECK(!(s.last.buttons & 0x10));

    sim_disconnect(sim);
    run(h, 100);
    CHECK(s.disconnects == 1);
    CHECK(!host_connected(h, s.slot));
    if (drop_every && !quiet) printf("  (%d packets dropped)\n", sim_dropped(sim));

    host_close(h);
    sim_free(sim);
    return g_fail == fail_before;
}

/* Many runs with different loss patterns: how often does pairing succeed? */
static void test_pairing_stress(void)
{
    int ok = 0, runs = 40, before = g_fail;
    unsigned seed;

    printf("pairing under 1-in-4 packet loss, %d runs\n", runs);
    for (seed = 1; seed <= (unsigned)runs; seed++) ok += test_pairing_seed(4, seed * 7919, 1);
    printf("  %d/%d runs fully passed\n", ok, runs);
    g_fail = before + (ok < runs);
}

static void test_reconnect(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a paired DualShock 4 reconnecting by itself\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_RECONNECT;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;

    /* The pad keeps the key it got when it was paired in the test before. */
    {
        FILE *f = fopen(DB, "rb");
        unsigned char rec[8 + 64];      /* magic, then the first record */
        int ok = f && fread(rec, 1, sizeof rec, f) == sizeof rec;
        if (f) fclose(f);
        CHECK(ok);
        if (!ok) return;
        sim = sim_new(&pad);
        sim_give_key(sim, rec + 8 + 6);
    }
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) return;

    sim_power_on(sim);
    run(h, 8000);
    CHECK(s.connects == 1);
    CHECK(sim_sdp_queries(sim) == 0);       /* ids came from the store */
    CHECK(s.states > 0 && (s.last.buttons & 0x4000));

    host_close(h);
    sim_free(sim);
}

/* An earlier run left its link to the pad open on the chip: paging the pad
 * answers "connection already exists" until the stale link is closed. */
static void test_stale_link(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a stale link from an earlier run is closed and the pad then pairs\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    sim = sim_new(&pad);
    sim_stale_link(sim, 0x34);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 60);
    run(h, 30000);
    CHECK(sim_stale_cleared(sim) == 1);
    CHECK(s.connects == 1);
    host_close(h);
    sim_free(sim);
}

static void test_unknown_pad_refused(void)
{
    static const unsigned char addr[6] = { 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a stranger connecting is ignored\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_RECONNECT;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    sim_power_on(sim);
    run(h, 3000);
    CHECK(s.connects == 0);
    CHECK(!sim_link_up(sim));
    host_close(h);
    sim_free(sim);
}

static void test_transport_lost(void)
{
    static const unsigned char addr[6] = { 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a controller that stops answering is noticed\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_RECONNECT;
    memcpy(pad.addr, addr, 6);
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    run(h, 30000);
    CHECK(!host_transport_lost(h));     /* idle but answering probes */
    sim_go_deaf(sim);
    run(h, 30000);
    CHECK(host_transport_lost(h));
    host_close(h);
    sim_free(sim);
}

/* A pad no profile knows, paired and read through its HID descriptor. */
static const unsigned char k_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x09, 0x39, 0x81, 0x42,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02,
    0x09, 0xC5, 0x09, 0xC4, 0x81, 0x02,
    0xC0,
};

static void test_generic_pad(void)
{
    static const unsigned char addr[6] = { 0x31, 0x32, 0x33, 0x34, 0x35, 0x36 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("pairing an unknown pad: generic profile from its descriptor\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x2DC8;
    pad.pid = 0x6001;
    memcpy(pad.desc, k_desc, sizeof k_desc);
    pad.desc_len = (int)sizeof k_desc;
    /* left stick right, hat south, button 1 (A), accelerator half */
    {
        static const unsigned char r[9] = { 0xFF, 0x80, 0x80, 0x80, 0x04, 0x01, 0x00, 0x00, 0x80 };
        memcpy(pad.report, r, sizeof r);
        pad.report_len = (int)sizeof r;
    }
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    host_pair(h, 30);
    run(h, 10000);
    CHECK(s.connects == 1);
    CHECK(s.info.profile && strcmp(s.info.profile, "Generic HID") == 0);
    CHECK(!host_output_ready(h, s.slot));
    CHECK(sim_sdp_continuations(sim) >= 1);     /* the descriptor came in pieces */
    CHECK(s.states > 0);
    CHECK(s.last.lx == 255 && s.last.ly == 128);
    CHECK((s.last.buttons & 0x40) && (s.last.buttons & 0x4000));    /* down, cross */
    CHECK(s.last.r2 == 0x80 && (s.last.buttons & 0x200));
    host_close(h);
    sim_free(sim);
}

/* A Switch Pro controller: simple mode until its wake-up sequence. */
static void test_switch_pad(void)
{
    static const unsigned char addr[6] = { 0x41, 0x42, 0x43, 0x44, 0x45, 0x46 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("pairing a Switch Pro controller: woken up into its full report\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x057E;
    pad.pid = 0x2009;
    pad.needs_output = 1;
    pad.basic[0] = 0x3F;                    /* simple mode: nothing pressed */
    pad.basic[3] = 8;
    pad.basic_len = 12;
    memset(pad.report, 0, 49);
    pad.report[0] = 0x30;
    pad.report[3] = 0x08;                   /* A */
    pad.report[6] = 0x00; pad.report[7] = 0x08; pad.report[8] = 0x80;  /* centred */
    pad.report[9] = 0x00; pad.report[10] = 0x08; pad.report[11] = 0x80;
    pad.report_len = 49;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    unlink("build/test-acl-diag.log");
    CHECK(log_open("build/test-acl-diag.log"));
    host_pair(h, 30);
    {
        long until = sim_now() + 10000;
        while (!s.connects && sim_now() < until) host_poll(h, 5);
        CHECK(s.connects == 1);
        CHECK(!host_output_ready(h, s.slot));
        CHECK(!host_rumble_pulse(h, s.slot, 500)); /* Initialization owns output. */
    }
    run(h, 10000);
    CHECK(s.connects == 1);
    CHECK(s.info.profile && strcmp(s.info.profile, "Switch Pro") == 0);
    CHECK(sim_outputs(sim) >= 4);           /* the whole wake-up sequence */
    CHECK(s.last.buttons & 0x2000);         /* A, from the full report: circle */
    CHECK(s.last.lx == 128 && s.last.ly == 128);
    {
        unsigned char garbage[2] = {0xa1, 0xff};
        char line[512];
        int i, acl = 0, frames = 0, unknown = 0, states = s.states;
        FILE *f;
        for (i = 0; i < 20; i++) sim_inject_acl(sim, 0x7fff, garbage, sizeof garbage);
        run(h, 500);
        CHECK(s.states > states);
        log_close();
        f = fopen("build/test-acl-diag.log", "r");
        CHECK(f != NULL);
        if (f) {
            while (fgets(line, sizeof line, f)) {
                if (strstr(line, "ACL sample")) acl++;
                if (strstr(line, "L2CAP frame")) frames++;
                if (strstr(line, "L2CAP unknown cid")) unknown++;
            }
            fclose(f);
        }
        CHECK(acl == 6 && frames == 6 && unknown == 2);
        unlink("build/test-acl-diag.log");
    }
    {
        static const unsigned char neutral[8] = { 0, 1, 0x40, 0x40, 0, 1, 0x40, 0x40 };
        unsigned char out[128];
        int before, states_before = s.states, i;
        pad_output stopped = {0};

        printf("Switch rumble: queued, rate-limited, stops and keeps input flowing\n");
        CHECK(host_output_ready(h, s.slot));
        before = sim_outputs(sim);
        for (i = 0; i < 100; i++) CHECK(host_rumble_pulse(h, s.slot, 500));
        CHECK(sim_outputs(sim) == before); /* API calls never send in a burst. */
        run(h, 100);
        CHECK(sim_outputs(sim) > before && sim_outputs(sim) <= before + 2);
        CHECK(sim_last_output(sim, out, sizeof out) == 11);
        CHECK(out[0] == 0xA2 && out[1] == 0x10);
        CHECK(memcmp(out + 3, neutral, sizeof neutral) != 0);
        run(h, 800);
        CHECK(sim_last_output(sim, out, sizeof out) == 11);
        CHECK(memcmp(out + 3, neutral, sizeof neutral) == 0);
        CHECK(s.states > states_before && (s.last.buttons & PAD_CIRCLE));
        CHECK(sim_output_min_gap(sim) >= 60);
        before = sim_output_attempts(sim);
        run(h, 500);
        CHECK(sim_output_attempts(sim) == before); /* No idle rumble traffic. */

        printf("Switch rumble: retry a lost stop, bound permanent failures\n");
        CHECK(host_rumble_pulse(h, s.slot, 100));
        run(h, 70);
        sim_fail_outputs(sim, 1);
        run(h, 500);
        CHECK(sim_last_output(sim, out, sizeof out) == 11);
        CHECK(memcmp(out + 3, neutral, sizeof neutral) == 0);

        CHECK(host_rumble_pulse(h, s.slot, 100));
        run(h, 70);
        sim_fail_outputs(sim, 100);
        before = sim_output_attempts(sim);
        run(h, 1000);
        CHECK(sim_output_attempts(sim) == before + 5);
        CHECK(sim_output_min_gap(sim) >= 60);
        CHECK(host_connected(h, s.slot)); /* Output failure does not drop input. */
        before = sim_output_attempts(sim);
        run(h, 1000);
        CHECK(sim_output_attempts(sim) == before);
        sim_fail_outputs(sim, 0);
        CHECK(host_set_output(h, s.slot, &stopped));
        run(h, 400);
        CHECK(sim_last_output(sim, out, sizeof out) == 11);
        CHECK(memcmp(out + 3, neutral, sizeof neutral) == 0);

        printf("game rumble: renew unchanged values, expire stale feedback, preserve web tests\n");
        {
            static const unsigned char motors[8] = { 0, 0x49, 0x40, 0x52, 0, 0x25, 0x40, 0x49 };
            CHECK(!host_rumble_feedback(NULL, s.slot, 64, 32));
            CHECK(!host_rumble_feedback(h, -1, 64, 32));
            for (i = 0; i < 10; i++) {
                CHECK(host_rumble_feedback(h, s.slot, 64, 32));
                run(h, 100);
            }
            CHECK(sim_last_output(sim, out, sizeof out) == 11);
            CHECK(memcmp(out + 3, motors, sizeof motors) == 0);
            CHECK(sim_output_min_gap(sim) >= 60);
            before = sim_output_attempts(sim);
            for (i = 0; i < 30; i++) {
                CHECK(host_rumble_feedback(h, s.slot, 0, 0));
                run(h, 20);
            }
            CHECK(sim_output_attempts(sim) == before + 5); /* Zeros do not restart stop retries. */
            CHECK(sim_last_output(sim, out, sizeof out) == 11);
            CHECK(memcmp(out + 3, neutral, sizeof neutral) == 0);
            CHECK(host_rumble_feedback(h, s.slot, 64, 32));
            run(h, 1000); /* Reader failure: no renewals, so rumble stops. */
            CHECK(sim_last_output(sim, out, sizeof out) == 11);
            CHECK(memcmp(out + 3, neutral, sizeof neutral) == 0);
            CHECK(host_connected(h, s.slot));

            CHECK(host_rumble_pulse(h, s.slot, 500));
            CHECK(host_rumble_feedback(h, s.slot, 0, 0));
            run(h, 100);
            CHECK(sim_last_output(sim, out, sizeof out) == 11);
            CHECK(memcmp(out + 3, neutral, sizeof neutral) != 0); /* Game zero did not cancel test. */
            for (i = 0; i < 5; i++) {
                CHECK(host_rumble_feedback(h, s.slot, 64, 32));
                run(h, 100);
            }
            CHECK(sim_last_output(sim, out, sizeof out) == 11);
            CHECK(memcmp(out + 3, motors, sizeof motors) == 0); /* Resume latest game values. */
            CHECK(sim_output_min_gap(sim) >= 60);
        }
        sim_disconnect(sim);
        run(h, 100);
        before = sim_output_attempts(sim);
        CHECK(!host_rumble_pulse(h, s.slot, 500));
        CHECK(!host_rumble_feedback(h, s.slot, 64, 32));
        run(h, 500);
        CHECK(sim_output_attempts(sim) == before);
    }
    host_close(h);
    sim_free(sim);
}

/* What the host changes in the shared controller, it puts back on close,
 * unless someone else changed it meanwhile. */
static void test_restore(void)
{
    static const unsigned char addr[6] = { 0x61, 0x62, 0x63, 0x64, 0x65, 0x66 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("the controller's page scan is put back on close, unless changed by another\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_RECONNECT;
    memcpy(pad.addr, addr, 6);
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h && sim_scan(sim) == 0x02);
    host_close(h);
    CHECK(sim_scan(sim) == 0x00);

    h = open_host(sim, &s);
    sim_set_scan(sim, 0x03);            /* the system turns on inquiry scan too */
    host_close(h);
    CHECK(sim_scan(sim) == 0x03);       /* left alone */
    sim_free(sim);
}

/* Garbage on every channel of a working link, and garbage events: the
 * host must neither fault nor lose its pad over it. */
static void test_garbage(void)
{
    static const unsigned char addr[6] = { 0x71, 0x72, 0x73, 0x74, 0x75, 0x76 };
    static const unsigned cids[] = { 0x0001, 0x0040, 0x0041, 0x0042, 0x0099 };
    static const unsigned char codes[] = { 0x03, 0x05, 0x06, 0x08, 0x0E, 0x0F, 0x13, 0x17, 0x18, 0x22, 0x2F, 0x31, 0x3E };
    unsigned seed = 777;
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;
    int i;

    printf("garbage frames and events on a working Classic link\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    host_pair(h, 30);
    run(h, 8000);
    CHECK(s.connects == 1);
    for (i = 0; i < 20000; i++) {
        unsigned char junk[300];
        int n, k;
        seed = seed * 1103515245u + 12345u;
        n = (int)((seed >> 8) % 280);
        for (k = 0; k < n; k++) {
            seed = seed * 1103515245u + 12345u;
            junk[k] = (unsigned char)(seed >> 16);
        }
        if (i % 4 == 3) sim_inject_event(sim, codes[(seed >> 4) % sizeof codes], junk, n < 255 ? n : 255);
        else sim_inject_acl(sim, cids[(seed >> 4) % 5], junk, n);
        if (i % 16 == 0) host_poll(h, 1);
    }
    run(h, 500);
    printf("  connects %d, disconnects %d, connected now %d\n", s.connects, s.disconnects,
           host_connected(h, s.slot));
    /* Garbage events can end the link (a Disconnection Complete for our
     * handle looks like any other); the books must still balance. */
    CHECK(host_connected(h, s.slot) == (s.connects - s.disconnects == 1));
    host_close(h);
    sim_free(sim);
}

/* A controller that never answers: the host gives up at once, and asks the
 * transport once why. */
static void test_dead_controller(void)
{
    static const unsigned char addr[6] = { 0x91, 0x92, 0x93, 0x94, 0x95, 0x96 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;
    int before = sim_diag_calls();

    printf("a controller that does not answer: host_open fails and the transport reports\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_RECONNECT;
    memcpy(pad.addr, addr, 6);
    sim = sim_new(&pad);
    sim_go_deaf(sim);
    h = open_host(sim, &s);
    CHECK(h == NULL);
    CHECK(sim_diag_calls() == before + 1);
    sim_free(sim);
}

static void test_sdp_parse(void)
{
    /* As a real pad answers: {{0x0200 ver, 0x0201 vid, 0x0202 pid}} */
    static const unsigned char list[] = {
        0x35, 0x14, 0x35, 0x12,
        0x09, 0x02, 0x00, 0x09, 0x01, 0x03,
        0x09, 0x02, 0x01, 0x09, 0x05, 0x4C,
        0x09, 0x02, 0x02, 0x09, 0x0C, 0xE6,
    };
    uint16_t vid = 0, pid = 0;

    printf("SDP Device ID parsing\n");
    CHECK(sdp_find_ids(list, sizeof list, &vid, &pid));
    CHECK(vid == 0x054C && pid == 0x0CE6);
    CHECK(!sdp_find_ids(list, 10, &vid, &pid));
}

static int identity_disconnects;
static int identity_command(void *ctx, unsigned opcode, const void *p, int len)
{
    (void)ctx; (void)p; (void)len;
    CHECK(opcode == OP_DISCONNECT);
    identity_disconnects++;
    return 1;
}

static void test_identity_publication(void)
{
    host_t h = {0};
    seen s = {0};
    static const hci_ops ops = {.cmd = identity_command};
    const unsigned char identity[6] = {2, 0xbc, 0x65, 0x1d, 0x10, 0xe0};
    unsigned char alias[6] = {2, 0xbc, 0x6b, 0x1d, 0x10, 0xe0};
    unsigned char report[12] = {0x3f, 1, 0, 8};
    link_t *bad, *good, *duplicate, *different;
    pad_info info;
    pad_state state;
    printf("identity deduplication: first valid input, LE aliases and distinct pads\n");
    h.hci.ops = &ops; h.ev.ud = &s;
    h.ev.on_connect = on_connect; h.ev.on_state = on_state;
    identity_disconnects = 0;
    bad = link_new(&h, alias);
    bad->le = bad->connected = 1;
    memcpy(bad->le_s.id_addr, identity, 6);
    bad->prof = profile_find(0x057e, 0x2009);
    link_ready(&h, bad);
    CHECK(!s.connects && !host_pad_get(&h, 0, &info, &state));
    good = link_new(&h, identity);
    good->connected = 1; good->prof = bad->prof;
    CHECK(classic_setup_pending(&h));
    good->disconnecting = 1;
    CHECK(!classic_setup_pending(&h));
    good->disconnecting = 0;
    link_ready(&h, good);
    CHECK(classic_setup_pending(&h));
    CHECK(!s.connects);
    link_input(&h, good, report, sizeof report);
    CHECK(s.connects == 1 && s.states == 1 && bad->disconnecting);
    CHECK(host_pad_get(&h, 1, &info, &state) && !memcmp(info.addr, identity, 6));
    link_input(&h, bad, report, sizeof report);
    CHECK(s.connects == 1 && s.states == 1);
    alias[2] = 0x70;
    duplicate = link_new(&h, alias);
    duplicate->le = duplicate->connected = 1; duplicate->prof = good->prof;
    memcpy(duplicate->le_s.id_addr, identity, 6);
    link_ready(&h, duplicate);
    link_input(&h, duplicate, report, sizeof report);
    CHECK(duplicate->disconnecting && s.connects == 1);
    alias[2] = 0x71;
    different = link_new(&h, alias);
    different->connected = 1; different->prof = good->prof;
    link_ready(&h, different);
    link_input(&h, different, report, sizeof report);
    CHECK(s.connects == 2 && !different->disconnecting);
    /* Identity may become known after input already started. */
    different->le = 1;
    memcpy(different->le_s.id_addr, identity, 6);
    link_input(&h, different, report, sizeof report);
    CHECK(different->disconnecting && identity_disconnects == 3);
}

static void test_bond_transport_isolation(void)
{
    host_t h = {0};
    const unsigned char addr[6] = {1, 2, 3, 4, 5, 6};
    snprintf(h.db_path, sizeof h.db_path, "build/test-isolated-bonds.db");
    db_rec *classic = db_put(&h, addr, 0);
    memset(classic->key, 0x11, 16);
    db_rec *le = db_put(&h, addr, 1);
    memset(le->key, 0x22, 16);
    CHECK(h.ndb == 2 && classic != le);
    CHECK(db_find(&h, addr, 0)->key[0] == 0x11);
    CHECK(db_find(&h, addr, 1)->key[0] == 0x22);
    db_forget(&h, addr, 1);
    CHECK(h.ndb == 1 && db_find(&h, addr, 0)->key[0] == 0x11);
    CHECK(!db_find(&h, addr, 1));
    db_put(&h, addr, 1);
    CHECK(host_forget(&h, addr) && h.ndb == 0);
    remove(h.db_path);
}

static void test_inquiry_backoff_and_pairing_order(void)
{
    sim_pad pad = {0};
    sim_t *sim = sim_new(&pad);
    host_t h = {0};
    const unsigned char addr[6] = {1, 2, 3, 4, 5, 6};
    unsigned char status[] = {0x0c, 1, OP_INQUIRY & 255, OP_INQUIRY >> 8};
    unsigned char found[15] = {1, 6, 5, 4, 3, 2, 1, 1, 0, 8, 0x25, 0, 0, 0, 0};
    h.hci = sim_hci(sim);
    h.last_rx = now_ms();
    h.inquiring = 1;
    sim_inject_event(sim, 0x0f, status, sizeof status);
    host_poll(&h, 0);
    CHECK(!h.inquiring && h.inquiry_retry_at > now_ms());
    h.pair_until = now_ms() + 60000;
    host_poll(&h, 0);
    CHECK(!h.inquiring); /* Rejected inquiry is not immediately retried. */

    link_t *l = link_new(&h, addr);
    l->le = l->connected = l->ready = l->disconnecting = 1;
    l->prof = profile_find(0x057e, 0x2009);
    /* A live LE setup blocks an arriving Classic discovery record.
     * Keep encryption waiting so this test sends no GATT requests. */
    l->disconnecting = 0;
    l->t_ready = l->t_conn = now_ms();
    l->le_s.bonded = 1;
    l->le_s.enc_sent = 1;
    l->le_s.t_enc = now_ms();
    db_put(&h, addr, 1);
    sim_inject_event(sim, 0x22, found, sizeof found);
    host_poll(&h, 0);
    CHECK(!h.links[1].used);
    CHECK(!h.inquiring);
    sim_free(sim);
}

static void test_first_input_timeout(void)
{
    sim_pad pad = {0};
    sim_t *sim = sim_new(&pad);
    host_t h = {0};
    const unsigned char addr[6] = {1, 2, 3, 4, 5, 6};
    h.hci = sim_hci(sim);
    h.last_rx = now_ms();
    link_t *l = link_new(&h, addr);
    l->connected = l->auth_ok = l->enc_on = 1;
    l->prof = profile_find(0x057e, 0x2009);
    link_ready(&h, l);
    l->t_ready = now_ms() - T_FIRST_INPUT;
    host_poll(&h, 0);
    CHECK(l->disconnecting);
    /* The same elapsed time must not disconnect a published controller. */
    memset(l, 0, sizeof *l);
    l = link_new(&h, addr);
    l->connected = l->auth_ok = l->enc_on = 1;
    l->prof = profile_find(0x057e, 0x2009);
    link_ready(&h, l);
    l->announced = 1;
    l->t_ready = now_ms() - T_FIRST_INPUT;
    host_poll(&h, 0);
    CHECK(!l->disconnecting);
    sim_free(sim);
}

static void test_input_diagnostics(void)
{
    host_t h = {0};
    link_t *l = &h.links[0];
    seen s = {0};
    unsigned char rep[12] = {0xff};
    char line[512];
    int i, samples = 0, first = 0, changes = 0;
    FILE *f;

    printf("input diagnostics: bounded samples without dropping input callbacks\n");
    unlink("build/test-input-diag.log");
    CHECK(log_open("build/test-input-diag.log"));
    l->prof = profile_find(0x057e, 0x2009);
    l->ready = 1;
    h.ev.ud = &s; h.ev.on_state = on_state;
    for (i = 0; i < 100; i++) link_input(&h, l, rep, sizeof rep);
    rep[0] = 0x3f; rep[3] = 8;
    for (i = 0; i < 100; i++) {
        rep[1] = (unsigned char)(i & 1);
        link_input(&h, l, rep, sizeof rep);
    }
    log_close();
    CHECK(s.states == 100 && l->diag_rx == 200 && l->diag_parsed == 100);
    f = fopen("build/test-input-diag.log", "r");
    CHECK(f != NULL);
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "input sample")) samples++;
            if (strstr(line, "first parsed input")) first++;
            if (strstr(line, "parsed buttons=")) changes++;
        }
        fclose(f);
    }
    CHECK(samples == 6 && first == 1 && changes == 4);
    unlink("build/test-input-diag.log");
}

int main(void)
{
    test_bond_transport_isolation();
    test_inquiry_backoff_and_pairing_order();
    test_first_input_timeout();
    test_identity_publication();
    test_input_diagnostics();
    test_sdp_parse();
    test_pairing(0);
    test_reconnect();
    test_pairing(7);
    test_stale_link();
    test_unknown_pad_refused();
    test_transport_lost();
    test_restore();
    test_dead_controller();
    test_garbage();
    test_generic_pad();
    test_switch_pad();
    test_pairing_stress();
    printf(g_fail ? "%d check(s) failed\n" : "all host checks passed\n", g_fail);
    return g_fail != 0;
}
