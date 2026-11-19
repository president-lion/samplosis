/* A tiny CLAP host for trying the macOS window (built by CMakeLists.txt as smp_gui_host):
     smp_gui_host <Samplosis.clap> [state.txt]
   It loads the plugin, applies the state file if one is given (the plugin's own text, as a host would restore a
   project), and opens the window in a Mac window of its own, so the window can be used with the keyboard and
   VoiceOver, or inspected through the accessibility interface, without a DAW. No audio runs. */
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <clap/clap.h>

static const clap_plugin_t *g_plugin;

static const void *h_ext(const clap_host_t *h, const char *id) { (void)h; (void)id; return NULL; }
static void h_restart(const clap_host_t *h) { (void)h; }
static void h_process(const clap_host_t *h) { (void)h; }
static void h_callback(const clap_host_t *h) {
  (void)h;
  dispatch_async(dispatch_get_main_queue(), ^{ if (g_plugin) g_plugin->on_main_thread(g_plugin); });
}
static const clap_host_t host = {CLAP_VERSION_INIT, NULL, "smp_gui_host", "Samplosis", "", "0.1",
                                 h_ext, h_restart, h_process, h_callback};

typedef struct { const char *p; size_t n, at; } In;
static int64_t in_read(const clap_istream_t *s, void *buf, uint64_t size) {
  In *in = s->ctx; size_t k = in->n - in->at < size ? in->n - in->at : (size_t)size;
  memcpy(buf, in->p + in->at, k); in->at += k; return (int64_t)k;
}

@interface Delegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
@implementation Delegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }
@end

int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 2) { fprintf(stderr, "usage: smp_gui_host <Samplosis.clap> [state.txt]\n"); return 2; }
    NSString *bpath = [NSString stringWithUTF8String:argv[1]];
    NSBundle *b = [NSBundle bundleWithPath:bpath];
    void *lib = dlopen(b ? b.executablePath.fileSystemRepresentation : argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "can't load: %s\n", dlerror()); return 1; }
    const clap_plugin_entry_t *entry = dlsym(lib, "clap_entry");
    if (!entry || !entry->init(argv[1])) { fprintf(stderr, "no clap_entry\n"); return 1; }
    const clap_plugin_factory_t *f = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t *d = f->get_plugin_descriptor(f, 0);
    g_plugin = f->create_plugin(f, &host, d->id);
    if (!g_plugin || !g_plugin->init(g_plugin)) { fprintf(stderr, "plugin didn't start\n"); return 1; }
    g_plugin->activate(g_plugin, 48000, 32, 1024);
    if (argc > 2) {
      NSData *t = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]];
      const clap_plugin_state_t *st = g_plugin->get_extension(g_plugin, CLAP_EXT_STATE);
      In in = {t.bytes, t.length, 0};
      clap_istream_t is = {&in, in_read};
      if (!st || !t || !st->load(g_plugin, &is)) fprintf(stderr, "the state wasn't taken\n");
    }
    const clap_plugin_gui_t *gui = g_plugin->get_extension(g_plugin, CLAP_EXT_GUI);
    uint32_t w = 0, h = 0;
    if (!gui || !gui->is_api_supported(g_plugin, CLAP_WINDOW_API_COCOA, false) ||
        !gui->create(g_plugin, CLAP_WINDOW_API_COCOA, false) || !gui->get_size(g_plugin, &w, &h)) {
      fprintf(stderr, "no Cocoa window\n"); return 1;
    }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    Delegate *del = [Delegate new];
    NSApp.delegate = del;
    NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, w, h)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                  backing:NSBackingStoreBuffered defer:NO];
    win.title = @"Samplosis";
    win.releasedWhenClosed = NO;
    clap_window_t cw = {CLAP_WINDOW_API_COCOA, {.cocoa = (__bridge void *)win.contentView}};
    if (!gui->set_parent(g_plugin, &cw)) { fprintf(stderr, "set_parent failed\n"); return 1; }
    gui->show(g_plugin);
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
    gui->destroy(g_plugin);
    g_plugin->deactivate(g_plugin);
    g_plugin->destroy(g_plugin);
    entry->deinit();
  }
  return 0;
}
