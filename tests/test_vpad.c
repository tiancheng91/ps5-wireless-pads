/* vpad_init against stand-ins for the console's libraries, to check the
 * order of the libScePad calls and what is fatal. Compiled on the host from
 * the console source itself. */
#include "../src/ps5_vpad.h"
#include "../src/util.h"


#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

/* What the stand-ins were asked, in order. */
static char g_calls[64];
static int g_inited, g_priv_result_after_init, g_init_result;
static uint64_t g_authid = 0x4800000000000001ull;
static uint8_t g_caps[16];

static long g_now = 100000;
static long fake_clock(void) { return g_now; }
static int g_klog_read = -1, g_klog_write = -1, g_device;
static int g_insert_calls, g_remote_calls, g_remote_available = 1;
static int32_t g_remote_result, g_remote_handle;
static uint8_t g_remote_strong, g_remote_weak;

int connect(int fd, const struct sockaddr *address, socklen_t length)
{
    (void)fd; (void)address; (void)length;
    errno = ECONNREFUSED; /* Keep tests away from any real local log server. */
    return -1;
}

int open(const char *path, int flags, ...)
{
    (void)flags;
    if (strcmp(path, "/dev/klog") == 0 && g_klog_read >= 0) return dup(g_klog_read);
    errno = ENOENT;
    return -1;
}

static void note(char c) { size_t n = strlen(g_calls); if (n < sizeof g_calls - 1) g_calls[n] = c; }

#define SCE_PAD_ERROR_NOT_INITIALIZED ((int32_t)0x80920005)

int32_t sceUserServiceInitialize(void *p) { (void)p; note('u'); return 0; }
static int32_t g_init_user = 0x10000000, g_fore_user = 0x10000000;
static int32_t g_login[4] = { 0x10000000, -1, -1, -1 };
int32_t sceUserServiceGetInitialUser(int32_t *u) { *u = g_init_user; return 0; }
int32_t sceUserServiceGetForegroundUser(int32_t *u) { *u = g_fore_user; return 0; }
int32_t sceUserServiceGetLoginUserIdList(int32_t list[4]) { memcpy(list, g_login, sizeof g_login); return 0; }
int32_t scePadInit(void) { note('I'); if (g_init_result == 0) g_inited = 1; return g_init_result; }
int32_t scePadSetProcessPrivilege(int32_t v)
{
    (void)v;
    note('P');
    return g_inited ? g_priv_result_after_init : SCE_PAD_ERROR_NOT_INITIALIZED;
}
static int g_pad_opened, g_open_result = 7, g_closed;
static unsigned g_pad_buttons;
static int g_pad_connected = 1;
int32_t scePadGetHandle(int32_t u, int32_t t, int32_t i)
{
    (void)u; (void)t; (void)i;
    return g_pad_opened ? 7 : (int32_t)0x80920008;      /* NO_HANDLE until opened */
}
static int32_t g_open_user;
int32_t scePadOpen(int32_t u, int32_t t, int32_t i, const void *p)
{
    int k, signed_in = 0;
    (void)t; (void)i; (void)p;
    g_open_user = u;
    for (k = 0; k < 4; k++) if (g_login[k] == u) signed_in = 1;
    if (!signed_in) return (int32_t)0x809B0081;         /* SCE_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN */
    if (g_open_result >= 0) g_pad_opened = 1;
    return g_open_result;
}
int32_t scePadClose(int32_t h) { (void)h; g_closed++; g_pad_opened = 0; return 0; }
int32_t scePadReadState(int32_t h, void *d)
{
    /* ScePadData: buttons at 0, connected at offset 76 (after the touch data) */
    unsigned char *p = d;
    if (h != 7) return (int32_t)0x80920003;
    memset(p, 0, 120);
    memcpy(p, &g_pad_buttons, 4);
    p[76] = (unsigned char)g_pad_connected;
    return 0;
}
int32_t scePadVirtualDeviceAddDevice(void *p, int32_t t)
{
    char line[160];
    int n;
    (void)p; (void)t;
    n = snprintf(line, sizeof line,
                 "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x%x][type:1][subType:22][capabilityBattery:0]\n",
                 0x10000 + ++g_device);
    if (g_klog_write >= 0) CHECK(write(g_klog_write, line, (size_t)n) == n);
    return 0;
}
int32_t scePadVirtualDeviceInsertData(int32_t h, const void *d) { (void)h; (void)d; g_insert_calls++; return 0; }
int32_t scePadVirtualDeviceDeleteDevice(int32_t h) { (void)h; return 0; }
uint64_t kernel_get_ucred_authid(pid_t pid) { (void)pid; return g_authid; }
int32_t kernel_set_ucred_authid(pid_t pid, uint64_t a) { (void)pid; g_authid = a; return 0; }
int32_t kernel_get_ucred_caps(pid_t pid, uint8_t c[16]) { (void)pid; memcpy(c, g_caps, 16); return 0; }
int32_t kernel_set_ucred_caps(pid_t pid, const uint8_t c[16]) { (void)pid; memcpy(g_caps, c, 16); return 0; }
int sceKernelSendNotificationRequest(int a, void *r, size_t n, int b) { (void)a; (void)r; (void)n; (void)b; return 0; }

static int g_bind_calls;
static int g_bind_result;
static int32_t g_bound_user;
static int32_t fake_bind(uint64_t d, int32_t u) { (void)d; g_bind_calls++; g_bound_user = u; return g_bind_result; }
static int32_t fake_remote(int32_t handle, void *setting)
{
    uint8_t *bytes = setting;
    g_remote_calls++;
    g_remote_handle = handle;
    memset(bytes, 0, 256);
    bytes[3] = g_remote_strong;
    bytes[4] = g_remote_weak;
    return g_remote_result;
}
void *dlopen(const char *path, int mode) { (void)path; (void)mode; return (void *)1; }
void *dlsym(void *h, const char *name)
{
    (void)h;
    if (strcmp(name, "scePadVirtualDeviceGetRemoteSetting") == 0)
        return g_remote_available ? (void *)fake_remote : NULL;
    return (void *)fake_bind;
}

static void reset(int init_result, int priv_after_init)
{
    memset(g_calls, 0, sizeof g_calls);
    g_inited = 0;
    g_init_result = init_result;
    g_priv_result_after_init = priv_after_init;
}

static void create_virtual(int slot)
{
    CHECK(vpad_add(slot));
    vpad_poll(g_now);
    g_now++;
    vpad_poll(g_now);           /* consume DEVICE_ADDED, then watch for ambiguity */
    g_now += 301;
    vpad_poll(g_now);
    CHECK(vpad_live(slot));
}

static void test_feedback(void)
{
    int fds[2], before;
    uint8_t strong = 0, weak = 0;
    pad_state st;

    printf("game feedback: only live virtual devices, pacing, failure isolation and reconnect\n");
    ph_clock_hook = fake_clock;
    reset(0, 0);
    CHECK(vpad_init());
    CHECK(!vpad_read_rumble(-1, g_now, &strong, &weak));
    CHECK(!vpad_read_rumble(0, g_now, &strong, &weak));
    CHECK(g_remote_calls == 0);
    if (pipe(fds) != 0) { CHECK(0); return; }
    g_klog_read = fds[0]; g_klog_write = fds[1];
    CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    unlink(VPAD_BIND_USER_PATH);
    create_virtual(0);
    CHECK(g_bind_calls == 0); /* System, not payload, assigns the player. */
    g_remote_strong = 200; g_remote_weak = 70;
    CHECK(vpad_read_rumble(0, g_now, &strong, &weak));
    CHECK(strong == 200 && weak == 70 && g_remote_handle == 0x10001);
    CHECK(!vpad_read_rumble(0, g_now + 15, &strong, &weak));
    CHECK(g_remote_calls == 1);
    g_now += 16;
    g_remote_strong = g_remote_weak = 0;
    CHECK(vpad_read_rumble(0, g_now, &strong, &weak));
    CHECK(!strong && !weak); /* Current values matter even when update flags are 0. */
    g_now += 16;
    g_remote_result = -1;
    CHECK(!vpad_read_rumble(0, g_now, &strong, &weak));
    before = g_remote_calls;
    g_now += 1000;
    CHECK(!vpad_read_rumble(0, g_now, &strong, &weak) && g_remote_calls == before);
    pad_state_reset(&st);
    before = g_insert_calls;
    vpad_update(0, &st);
    CHECK(vpad_live(0) && g_insert_calls == before + 1);
    vpad_remove(0);
    CHECK(!vpad_read_rumble(0, g_now, &strong, &weak));
    g_remote_result = 0;
    {
        FILE *f = fopen(VPAD_BIND_USER_PATH, "w");
        CHECK(f != NULL);
        if (f) { fputs("main\n", f); fclose(f); }
    }
    create_virtual(0);
    CHECK(g_bind_calls == 1 && g_bound_user == g_init_user);
    unlink(VPAD_BIND_USER_PATH);
    CHECK(vpad_read_rumble(0, g_now, &strong, &weak));
    CHECK(g_remote_handle == 0x10002);
    g_remote_available = 0;
    reset(0, 0);
    CHECK(vpad_init()); /* Missing optional API never prevents input initialization. */
    before = g_remote_calls;
    g_now += 100;
    CHECK(!vpad_read_rumble(0, g_now, &strong, &weak) && g_remote_calls == before);
    CHECK(vpad_live(0));
    vpad_remove(0);
    {
        FILE *f = fopen(VPAD_BIND_USER_PATH, "w");
        CHECK(f != NULL);
        if (f) { fputs("main\n", f); fclose(f); }
        g_bind_result = -1;
        create_virtual(0);
        CHECK(vpad_live(0) && g_bind_calls == 2); /* Failed override keeps system selection possible. */
        vpad_remove(0);
        unlink(VPAD_BIND_USER_PATH);
        g_bind_result = 0;
    }
    {
        int32_t saved[4];
        memcpy(saved, g_login, sizeof saved);
        memset(g_login, 0xff, sizeof g_login);
        CHECK(vpad_init());
        create_virtual(0);
        CHECK(vpad_live(0) && g_bind_calls == 2); /* No login required before Home. */
        vpad_remove(0);
        memcpy(g_login, saved, sizeof saved);
    }
    close(fds[0]); close(fds[1]);
    g_klog_read = g_klog_write = -1;
}

int main(void)
{
    printf("vpad_init: libScePad is initialised before its privilege is set\n");
    reset(0, 0);
    CHECK(vpad_init() == 1);
    CHECK(strstr(g_calls, "IP") != NULL);               /* init, then privilege */
    CHECK(strchr(g_calls, 'P') > strchr(g_calls, 'I'));

    printf("a privilege answer that is not 0 is a warning, as in the references\n");
    reset(0, SCE_PAD_ERROR_NOT_INITIALIZED);
    CHECK(vpad_init() == 1);

    printf("a failing scePadInit is fatal, and the privilege is never set\n");
    reset((int32_t)0x80920001, 0);
    CHECK(vpad_init() == 0);
    CHECK(strchr(g_calls, 'P') == NULL);

    printf("the original credentials can be put back and raised again\n");
    reset(0, 0);
    CHECK(vpad_init() == 1);
    CHECK(g_authid == 0x3800000000010003ull);               /* raised */
    CHECK(vpad_creds_restore() == 1);
    CHECK(g_authid == 0x4800000000000001ull && g_caps[0] == 0x00);   /* as found at the start */
    CHECK(vpad_creds_restore() == 1);                       /* twice is harmless */
    CHECK(vpad_creds_raise() == 1);
    CHECK(g_authid == 0x3800000000010003ull);

    printf("the user's pad: no handle, so it is opened the normal way; read; closed at the end\n");
    {
        uint32_t b = 0;
        ph_clock_hook = fake_clock;
        reset(0, 0);
        CHECK(vpad_init() == 1);
        g_pad_buttons = 0x00104400u;                        /* L1 + R1 + touchpad */
        CHECK(vpad_read_physical(&b) == 1);
        CHECK(g_pad_opened == 1 && b == 0x00104400u);
        CHECK(vpad_physical_error() == 0);
        g_pad_buttons = 0x80000000u | 0x400;                /* the system has the input */
        CHECK(vpad_read_physical(&b) == 0 && vpad_physical_error() == (int32_t)0xFFFF0003);
        g_pad_buttons = 0;
        g_pad_connected = 0;
        CHECK(vpad_read_physical(&b) == 0 && vpad_physical_error() == (int32_t)0xFFFF0002);
        g_pad_connected = 1;
        vpad_close_physical();
        CHECK(g_closed == 1 && g_pad_opened == 0);
        g_pad_opened = 0;
        g_open_result = (int32_t)0x80920004;                /* opening is refused */
        CHECK(vpad_read_physical(&b) == 0);                 /* tried again at most every 5 s: */
        CHECK(vpad_physical_error() == (int32_t)0x80920008);
        g_now += 6000;
        CHECK(vpad_read_physical(&b) == 0);
        CHECK(vpad_physical_error() == (int32_t)0x80920004);
        g_open_result = 7;
    }

    printf("the user given the pads is one who is signed in\n");
    {
        uint32_t b = 0;
        ph_clock_hook = fake_clock;

        /* the initial user is signed in: preferred over the foreground one */
        g_init_user = 0x2000; g_fore_user = 0x3000;
        g_login[0] = 0x3000; g_login[1] = 0x2000; g_login[2] = g_login[3] = -1;
        g_pad_opened = 0; g_closed = 0; g_now += 10000;
        reset(0, 0);
        CHECK(vpad_init() == 1);
        vpad_close_physical();
        g_pad_opened = 0;
        g_now += 10000;
        CHECK(vpad_read_physical(&b) == 1 && g_open_user == 0x2000);

        /* the initial user is not signed in; the foreground one is */
        vpad_close_physical();
        g_pad_opened = 0;
        g_login[0] = 0x3000; g_login[1] = g_login[2] = g_login[3] = -1;
        reset(0, 0);
        g_now += 10000;
        CHECK(vpad_init() == 1);                   /* picks again */
        vpad_close_physical();
        g_pad_opened = 0;
        g_now += 10000;
        CHECK(vpad_read_physical(&b) == 1 && g_open_user == 0x3000);

        /* neither: the first one that is signed in */
        vpad_close_physical();
        g_pad_opened = 0;
        g_init_user = 0x2000; g_fore_user = 0x2500;
        g_login[0] = 0x4000; g_login[1] = g_login[2] = g_login[3] = -1;
        reset(0, 0);
        g_now += 10000;
        CHECK(vpad_init() == 1);
        vpad_close_physical();
        g_pad_opened = 0;
        g_now += 10000;
        CHECK(vpad_read_physical(&b) == 1 && g_open_user == 0x4000);

        /* nobody signed in: no pad can be read, and no user is made up */
        vpad_close_physical();
        g_pad_opened = 0;
        g_login[0] = g_login[1] = g_login[2] = g_login[3] = -1;
        reset(0, 0);
        g_now += 10000;
        CHECK(vpad_init() == 1);
        g_now += 10000;
        CHECK(vpad_read_physical(&b) == 0 && g_pad_opened == 0);

        /* USER_NOT_LOGIN on open: the user is looked for again, at the next try */
        g_init_user = 0x2000;
        g_login[0] = 0x2000;
        g_now += 10000;
        CHECK(vpad_read_physical(&b) == 1 && g_open_user == 0x2000);
        vpad_close_physical();
        g_pad_opened = 0;
        g_init_user = 0x10000000; g_fore_user = 0x10000000; g_login[0] = 0x10000000;
    }

    printf("vpad_choose_user: default order, and the bind_user override\n");
    {
        int32_t two[4] = { 0x1111, 0x2222, -1, -1 };
        int32_t one[4] = { 0x1111, -1, -1, -1 };
        int32_t none[4] = { -1, -1, -1, -1 };

        CHECK(vpad_choose_user(two, 0x1111, 0x2222, NULL) == 0x1111);       /* the owner */
        CHECK(vpad_choose_user(two, 0x1111, 0x2222, "") == 0x1111);
        CHECK(vpad_choose_user(two, 0x1111, 0x2222, "other") == 0x2222);    /* someone else */
        CHECK(vpad_choose_user(one, 0x1111, 0x1111, "other") == 0x1111);    /* nobody else: default */
        CHECK(vpad_choose_user(two, 0x1111, 0x1111, "2222") == 0x2222);     /* an id */
        CHECK(vpad_choose_user(two, 0x1111, 0x1111, "9999") == 0x1111);     /* not signed in: default */
        CHECK(vpad_choose_user(two, 0x1111, 0x1111, "zz") == 0x1111);       /* not an id: default */
        CHECK(vpad_choose_user(none, 0x1111, 0x1111, "other") == -1);       /* nobody signed in */
    }

    test_feedback();
    printf(g_fail ? "%d check(s) failed\n" : "all vpad checks passed\n", g_fail);
    return g_fail != 0;
}
