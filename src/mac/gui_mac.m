/* placeholder until the Cocoa window is written */
#include "plugin_int.h"
static bool g_supported(const clap_plugin_t *p, const char *api, bool floating) { (void)p; (void)api; (void)floating; return false; }
static bool g_preferred(const clap_plugin_t *p, const char **api, bool *floating) { (void)p; (void)api; (void)floating; return false; }
static bool g_create(const clap_plugin_t *p, const char *api, bool floating) { return false; }
static void g_destroy(const clap_plugin_t *p) {}
static bool g_scale(const clap_plugin_t *p, double s) { return false; }
static bool g_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { return false; }
static bool g_can_resize(const clap_plugin_t *p) { return false; }
static bool g_hints(const clap_plugin_t *p, clap_gui_resize_hints_t *h) { return false; }
static bool g_adjust(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { return false; }
static bool g_set_size(const clap_plugin_t *p, uint32_t w, uint32_t h) { return false; }
static bool g_set_parent(const clap_plugin_t *p, const clap_window_t *w) { return false; }
static bool g_set_transient(const clap_plugin_t *p, const clap_window_t *w) { return false; }
static void g_title(const clap_plugin_t *p, const char *t) {}
static bool g_show(const clap_plugin_t *p) { return false; }
static bool g_hide(const clap_plugin_t *p) { return false; }
const clap_plugin_gui_t smp_gui_ext = {g_supported, g_preferred, g_create, g_destroy, g_scale, g_size, g_can_resize,
                                        g_hints, g_adjust, g_set_size, g_set_parent, g_set_transient, g_title, g_show, g_hide};
void smp_gui_close(Plug *P) {}
void smp_gui_loaded(Plug *P) {}
