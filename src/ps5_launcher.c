/* SPDX-License-Identifier: GPL-3.0-or-later
 * Web-shortcut installation follows itsPLK/ps5-payload-manager,
 * src/app_installer.c: resolve TitleDir by NID, otherwise use AppInstallAll. */
#include "ps5_launcher.h"
#include "launcher.h"
#include "log.h"

#include <stdint.h>
#include <sys/types.h>

/* SDK declarations, also supplied by the host test's stand-ins. */
int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle);
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid);
int sceAppInstUtilAppInstallAll(void *reserved);

int ps5_launcher_register(void)
{
    int (*title_dir)(const char *, const char *, void *) = NULL;
    uint32_t handle;

    if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0)
        title_dir = (int (*)(const char *, const char *, void *))
            kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
    if (title_dir) {
        log_line("launcher: registering %s through AppInstallTitleDir", LAUNCHER_TITLE_ID);
        return title_dir(LAUNCHER_TITLE_ID, LAUNCHER_APP_ROOT "/", NULL);
    }
    log_line("launcher: AppInstallTitleDir unavailable; requesting AppInstallAll scan");
    return sceAppInstUtilAppInstallAll(NULL);
}
