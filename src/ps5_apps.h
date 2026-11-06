/* Registers (or unregisters) the PadBridge entry in "Contenido multimedia" with
 * the console, the way Payload Manager does for its own. */
#ifndef PADBRIDGE_PS5_APPS_H
#define PADBRIDGE_PS5_APPS_H

#ifndef APPS_LAUNCHER_STAMP
#define APPS_LAUNCHER_STAMP "/data/padbridge/launcher-installed"
#endif

/* Writes the files if needed and asks the system to register the entry.
 * 0 on success, -1 on failure. */
int apps_install_launcher(void);

/* Asks the system to remove the entry and deletes the files. 0 on success. */
int apps_remove_launcher(void);

#endif
