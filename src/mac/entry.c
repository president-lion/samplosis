/* The exported CLAP entry of the macOS plugins: the functions are in the static library (plugin.c, built with
   SMP_CLAP_FIRST), and clap-wrapper builds this file into the CLAP, and links it into the VST3 and Audio Unit. */
#include <clap/clap.h>
bool smp_entry_init(const char *path);
void smp_entry_deinit(void);
const void *smp_entry_factory(const char *id);
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, smp_entry_init, smp_entry_deinit, smp_entry_factory};
