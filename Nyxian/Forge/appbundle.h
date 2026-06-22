/*
 * appbundle.h — assemble a compiled binary into a runnable `.app` bundle.
 *
 * The step between "forge compiled a Mach-O" and "run it as an app": lay out
 *   <Name>.app/<Name>        (the executable)
 *   <Name>.app/Info.plist    (generated)
 *   <Name>.app/<resource>... (copied resources)
 * Pure file I/O + plist text, so it is host-verifiable. Signing (ZSign) and
 * launching as a guest (LiveContainer) are the iOS seams that take it from here.
 */
#ifndef FORGE_APPBUNDLE_H
#define FORGE_APPBUNDLE_H

#include <stddef.h>

typedef struct {
    const char *name;        /* CFBundleExecutable + display name */
    const char *bundle_id;   /* CFBundleIdentifier */
    const char *version;     /* CFBundleShortVersionString / Version (e.g. "1.0") */
    const char *min_os;      /* MinimumOSVersion (e.g. "16.0") */
} AppBundleSpec;

/* Assemble `<out_dir>/<name>.app` from `executable`, copying `resources` (an
 * array of `resource_count` file paths) into the bundle root. Returns the .app
 * path (malloc'd; caller frees) or NULL on failure. */
char *forge_assemble_app(const char *out_dir, const char *executable,
                         AppBundleSpec spec,
                         const char *const *resources, size_t resource_count);

#endif /* FORGE_APPBUNDLE_H */
