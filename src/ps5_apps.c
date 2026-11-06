#include "ps5_apps.h"
#include "launcher.h"
#include "ps5_launcher.h"
#include "log.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

extern const unsigned char icon_png_data[];
extern const unsigned int icon_png_data_len;

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppUnInstall(const char *title_id);

int apps_install_launcher(void)
{
    int w, r;

    w = launcher_write(LAUNCHER_APP_ROOT, icon_png_data, icon_png_data_len);
    if (w < 0) {
        log_line("launcher: could not write the files under %s", LAUNCHER_APP_ROOT);
        return -1;
    }
    if (w == 0 && access(APPS_LAUNCHER_STAMP, F_OK) == 0) return 0;
    /* A successful previous version must not hide a failed update. */
    if (unlink(APPS_LAUNCHER_STAMP) != 0 && errno != ENOENT) {
        log_line("launcher: cannot clear registration marker");
        return -1;
    }
    if ((r = sceAppInstUtilInitialize()) != 0) {
        log_line("launcher: sceAppInstUtilInitialize -> %#x", (unsigned)r);
        return -1;
    }
    /* Existing icon files alone do not prove successful registration. */
    r = ps5_launcher_register();
    sceAppInstUtilTerminate();
    log_line("launcher: registration of %s -> %#x", LAUNCHER_TITLE_ID, (unsigned)r);
    if (r == 0 || (uint32_t)r == 0x80990002u) {
        FILE *stamp = fopen(APPS_LAUNCHER_STAMP, "w");
        if (stamp) {
            if (fclose(stamp)) log_line("launcher: registration marker close failed");
        } else log_line("launcher: cannot save registration marker; will retry next start");
        return 0;
    }
    return -1;
}

int apps_remove_launcher(void)
{
    int r = sceAppInstUtilInitialize();

    if (r == 0) {
        r = sceAppInstUtilAppUnInstall(LAUNCHER_TITLE_ID);
        sceAppInstUtilTerminate();
    }
    log_line("launcher: removed %s -> %#x", LAUNCHER_TITLE_ID, (unsigned)r);
    launcher_remove_files(LAUNCHER_APP_ROOT);
    unlink(APPS_LAUNCHER_STAMP);
    return r == 0 ? 0 : -1;
}
