#include "ps5_apps.h"
#include "launcher.h"
#include "ps5_launcher.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

const unsigned char icon_png_data[] = {0};
const unsigned int icon_png_data_len = 1;
static int fails, writes, registrations, initializations, terminations;
static int file_result, init_result, register_result;
static int handle_result, legacy_available = 1, resolutions, all_calls;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
void log_line(const char *fmt, ...) { (void)fmt; }
int launcher_write(const char *root, const unsigned char *icon, size_t len)
{
    CHECK(!strcmp(root, LAUNCHER_APP_ROOT) && icon == icon_png_data && len == 1);
    writes++;
    return file_result;
}
int launcher_remove_files(const char *root) { (void)root; return 0; }
int sceAppInstUtilInitialize(void) { initializations++; return init_result; }
int sceAppInstUtilTerminate(void) { terminations++; return 0; }
int sceAppInstUtilAppUnInstall(const char *title) { (void)title; return 0; }
static int title_dir(const char *title, const char *root, void *reserved)
{
    CHECK(!strcmp(title, LAUNCHER_TITLE_ID) && !strcmp(root, LAUNCHER_APP_ROOT "/") && !reserved);
    registrations++;
    return register_result;
}
int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle)
{
    CHECK(pid == -1 && !strcmp(basename, "libSceAppInstUtil.sprx"));
    *handle = 42;
    return handle_result;
}
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid)
{
    CHECK(pid == -1 && handle == 42 && !strcmp(nid, "Wudg3Xe3heE"));
    resolutions++;
    return legacy_available ? (intptr_t)title_dir : 0;
}
int sceAppInstUtilAppInstallAll(void *reserved)
{
    CHECK(!reserved);
    all_calls++; registrations++;
    return register_result;
}

int main(void)
{
    unlink(APPS_LAUNCHER_STAMP);
    printf("unchanged files without a success marker retry registration\n");
    file_result = 0; register_result = -1;
    CHECK(apps_install_launcher() == -1 && registrations == 1);
    CHECK(access(APPS_LAUNCHER_STAMP, F_OK) != 0);
    register_result = 0;
    CHECK(apps_install_launcher() == 0 && registrations == 2);
    CHECK(access(APPS_LAUNCHER_STAMP, F_OK) == 0);
    printf("successful registration avoids repeated global scans\n");
    CHECK(apps_install_launcher() == 0 && registrations == 2);
    printf("failed file update clears old success before retrying\n");
    file_result = 1; register_result = -1;
    CHECK(apps_install_launcher() == -1 && registrations == 3);
    CHECK(access(APPS_LAUNCHER_STAMP, F_OK) != 0);
    file_result = 0; register_result = (int)UINT32_C(0x80990002);
    CHECK(apps_install_launcher() == 0 && registrations == 4);
    CHECK(initializations == 4 && terminations == 4 && all_calls == 0);
    unlink(APPS_LAUNCHER_STAMP);
    printf("file/initialization failure skips registration\n");
    file_result = -1;
    CHECK(apps_install_launcher() == -1 && registrations == 4 && initializations == 4);
    file_result = 1; init_result = -1;
    CHECK(apps_install_launcher() == -1 && registrations == 4 && terminations == 4);
    printf("missing TitleDir NID or module handle uses public scan fallback\n");
    init_result = 0; register_result = 0; legacy_available = 0;
    CHECK(apps_install_launcher() == 0 && all_calls == 1 && resolutions == 5);
    unlink(APPS_LAUNCHER_STAMP);
    handle_result = -1;
    CHECK(apps_install_launcher() == 0 && all_calls == 2 && resolutions == 5);
    unlink(APPS_LAUNCHER_STAMP);
    register_result = -1;
    CHECK(apps_install_launcher() == -1 && all_calls == 3);
    CHECK(initializations == 8 && terminations == 7);
    CHECK(access(APPS_LAUNCHER_STAMP, F_OK) != 0);
    CHECK(writes == 10);
    printf(fails ? "apps: %d FAILED\n" : "apps: all ok\n", fails);
    return fails != 0;
}
