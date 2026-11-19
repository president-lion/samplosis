/* gui_mac.m: Samplosis's window on macOS, made for VoiceOver and the keyboard. The Mac counterpart of gui.c: the same
   pages, controls, Tab order, words and behaviour, with native Cocoa controls that VoiceOver reads as they are.

   - Every control carries its own accessible name (the label before it); the visible labels are hidden from
     VoiceOver so nothing is read twice.
   - Values are sliders whose accessible value is the words ("-6.0 dB", "120 ms", "C3"). Up or Right and Down or Left
     step a value (Shift ten times as far, Option or Control a tenth), Page Up and Page Down ten steps, Home and End
     go to the limits. VoiceOver's increment and decrement (VO-Up, VO-Down) step it too. Return, or typing a
     character, puts an edit box in its place holding the value; Return there reads what was typed (a bad entry
     beeps and the old value stays), Escape leaves it as it was, Tab or leaving the box reads it too.
   - Choices are pop-up buttons. Up and Down step through the choices without opening the menu (each step applies,
     as the Windows combo boxes do); Space or Return opens it.
   - Player is a segmented control, Sampler or SoundFont; Left and Right switch, Ctrl+Tab and Ctrl+Shift+Tab switch
     from any control on a main page.
   - Tab and Shift+Tab go through the page's controls in order (hidden ones skipped), whatever the Mac's keyboard
     navigation setting is; past either end they wrap.
   - A file that won't load beeps and its reason is announced (VoiceOver) as well as shown in Status.
   A 250 ms timer brings the controls up to date (automation, renders, background loads) and frees replaced samples;
   a value is set only when it changes. Everything here runs on the main thread. */
#import <Cocoa/Cocoa.h>
#include <math.h>
#include <string.h>
#include "plugin_int.h"
#include "sfknob.h"
#include "decode.h"

#define W_WIDTH 760
#define ROW 30
#define LIMIT_LO 0.1
#define LIMIT_HI 10000.0
#define NOTE_MS 6000
#define NCH 16
#define NKNOB 16
#define SL_LIMIT (-1)
#define SL_KNOB0 (-2)
enum { PG_SAMPLER = 0, PG_SETTINGS = 1, PG_FONT = 2, NPAGES = 3 };
enum { LOAD_SAMPLE, LOAD_FONT, LOAD_DROPPED };
#define N_REST (SMP_NPARAMS - 2)
#define REST_ROWS ((N_REST + 1) / 2)
#define W_HEIGHT_S (8 + (4 + REST_ROWS + 2) * ROW + 8)
#define W_HEIGHT_F (8 + 23 * ROW + 26 + 8)
#define W_HEIGHT (W_HEIGHT_S > W_HEIGHT_F ? W_HEIGHT_S : W_HEIGHT_F)

@class SmpGui;
struct Gui { void *ctl; };                     /* the controller, retained (CFBridgingRetain) */

/* ---- tests: beeps are counted, and can be kept quiet (as in gui.c) ---- */
static volatile LONG beeps; static int quiet;
SMP_API long smp_test_gui_beeps(void) { return beeps; }
SMP_API void smp_test_gui_quiet(int q) { quiet = q; }
static void beep(void) { InterlockedIncrement(&beeps); if (!quiet) NSBeep(); }

static NSHashTable *g_views;                   /* (see "VoiceOver's focus" below) */
static int g_panels;

static NSString *ns(const char *s) { NSString *r = s ? [NSString stringWithUTF8String:s] : nil; return r ? r : @""; }

/* ================================================================ controls */
@protocol SmpKeys
- (BOOL)smpKey:(NSEvent *)e from:(NSView *)v;       /* the controller's keys first; YES when it took the key */
@end

@interface SmpView : NSView
@property (weak) SmpGui *gui;
@end

@interface SmpSlider : NSSlider
@property (weak) SmpGui *gui;
@property int what;                                 /* a parameter id, SL_LIMIT, or SL_KNOB0 - knob */
@property (copy) NSString *said;                    /* the words now */
@property (strong) NSTextField *ed;                 /* the edit box that stands in for it */
@property BOOL editing;
@end

@interface SmpPopUp : NSPopUpButton <NSMenuDelegate>
@property (weak) SmpGui *gui;
@property BOOL menuOpen;
@end

@interface SmpButton : NSButton
@property (weak) SmpGui *gui;
@end

@interface SmpSegments : NSSegmentedControl
@property (weak) SmpGui *gui;
@end

@interface SmpField : NSTextField
@property (weak) SmpGui *gui;
@property (weak) SmpSlider *forSlider;              /* the slider it types for, or nil */
@end

/* ================================================================ the controller */
typedef struct { int one, bank, program, channel, pin[NCH], out[NCH]; } SfCfg;

@interface SmpGui : NSObject <NSTextFieldDelegate, SmpKeys>
{
@public
  Plug *P;
  int page;
  BOOL setting_tab, closing, sf_one;
  int modal;
  long fails_seen, notes_seen;
  char note[1900]; ULONGLONG note_until;
  char dir_shown[1024], used_text[700], ff_text[1100], ff_for[1024]; int ff_searched;
  int nknobs; char knob_key[2200];
  int transpose_y;
  const void *list_font; char list_key[1200];
  char from_song[NCH][300];
  char lacks_preset[200], lacks_chan[NCH][200];
  char file_key[2][1110]; char file_dir[2][1100]; BOOL file_live[2];
}
@property (strong) SmpView *view;
@property (strong) NSView *pageView0, *pageView1, *pageView2;
@property (strong) SmpSegments *tabs;
@property (strong) NSMutableArray *order0, *order1, *order2;   /* each page's Tab order */
@property (strong) NSTimer *timer;
@property (strong) id keyMonitor;                   /* Ctrl+Tab, which AppKit would take for its own focus moves */
@property (strong) NSMutableArray *sliders;
/* the Sampler page */
@property (strong) SmpField *sample, *status;
@property (strong) SmpButton *browse, *settings;
@property (strong) SmpPopUp *file0, *file1;
/* parameters: popup or slider per id */
@property (strong) NSMutableDictionary *valOf;       /* @(id) -> control */
/* the SoundFont page */
@property (strong) SmpField *sffile, *sfstatus;
@property (strong) SmpButton *sfbrowse, *sfsettings;
@property (strong) SmpPopUp *sfmode, *sfpreset, *sfchannel;
@property (strong) NSMutableArray *chan, *outp;      /* SmpPopUp x 16 */
@property (strong) NSMutableArray *modeOne, *modeAll;   /* views each mode shows (labels too) */
@property (strong) NSMutableArray *knob, *knobLab;   /* SmpSlider and NSTextField x NKNOB */
@property (strong) NSTextField *fxLab, *sfstatusLab;
/* Settings */
@property (strong) SmpPopUp *stretcher, *keeptone, *rawrate, *rawch, *rawfmt, *rawend;
@property (strong) SmpField *dir, *ffmpeg, *used;
@property (strong) SmpButton *dirbrowse, *ffbrowse, *clear, *back;
@property (strong) SmpSlider *limit;
@property (weak) NSView *last;
- (void)refresh;
- (void)tabFrom:(NSView *)v back:(BOOL)back;
- (Plug *)plug;
- (void)loadPath:(NSString *)path how:(int)how;
- (void)nudge:(SmpSlider *)s by:(double)by;
- (void)focusedFile:(SmpPopUp *)p;
- (void)fileList:(int)kind rescan:(BOOL)rescan;
- (void)editEnd:(SmpSlider *)s apply:(BOOL)apply toSlider:(BOOL)toSlider;
- (void)nextTab:(BOOL)back;
- (void)takeFocus;
@end

/* ---- control classes: keys go to the controller first ---- */
static NSView *owner_of(NSResponder *r) {          /* a text field's field editor stands for the field */
  if ([r isKindOfClass:[NSTextView class]] && [(NSTextView *)r isFieldEditor]) {
    id d = [(NSTextView *)r delegate];
    if ([d isKindOfClass:[NSView class]]) return d;
  }
  return [r isKindOfClass:[NSView class]] ? (NSView *)r : nil;
}

@implementation SmpView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return NO; }
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)s {
  return [[s draggingPasteboard] canReadObjectForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}] ? NSDragOperationCopy : NSDragOperationNone;
}
@end

@implementation SmpSlider
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return !self.hidden; }
- (void)keyDown:(NSEvent *)e { if (![(id<SmpKeys>)self.gui smpKey:e from:self]) [super keyDown:e]; }
- (BOOL)becomeFirstResponder { BOOL ok = [super becomeFirstResponder]; if (ok) self.gui.last = self; return ok; }
- (NSString *)accessibilityValueDescription { return self.said ?: @""; }
- (id)accessibilityValue { return self.said ?: @""; }
@end

@implementation SmpPopUp
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return !self.hidden; }
- (void)keyDown:(NSEvent *)e { if (![(id<SmpKeys>)self.gui smpKey:e from:self]) [super keyDown:e]; }
- (void)menuWillOpen:(NSMenu *)m { (void)m; self.menuOpen = YES; }
@end

@implementation SmpButton
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return !self.hidden; }
- (void)keyDown:(NSEvent *)e { if (![(id<SmpKeys>)self.gui smpKey:e from:self]) [super keyDown:e]; }
- (BOOL)becomeFirstResponder { BOOL ok = [super becomeFirstResponder]; if (ok) self.gui.last = self; return ok; }
@end

@implementation SmpSegments
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return !self.hidden; }
- (void)keyDown:(NSEvent *)e { if (![(id<SmpKeys>)self.gui smpKey:e from:self]) [super keyDown:e]; }
- (BOOL)becomeFirstResponder { BOOL ok = [super becomeFirstResponder]; if (ok) self.gui.last = self; return ok; }
@end

@implementation SmpField
- (BOOL)acceptsFirstResponder { return !self.hidden && (self.isEditable || self.isSelectable); }
- (BOOL)canBecomeKeyView { return [self acceptsFirstResponder]; }
- (BOOL)becomeFirstResponder {
  BOOL ok = [super becomeFirstResponder];
  if (ok && !self.forSlider) self.gui.last = self;
  return ok;
}
@end

/* ================================================================ values (as gui.c) */
static BOOL is_choice(int id) { int k = smp_param_info(id)->kind; return k == SMP_K_CHOICE || k == SMP_K_BOOL; }
static void param_text(int id, double v, char *out, int cap) {
  smp_value_text(id, v, out, cap);
  if (is_choice(id) && out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
}
static void gb_text(double gb, char *out, int cap) {
  if (fabs(gb - floor(gb + 0.5)) < 1e-9) snprintf(out, (size_t)cap, "%.0f GB", gb);
  else snprintf(out, (size_t)cap, "%.1f GB", gb);
}
static BOOL knob_whole(const SmpKnobInfo *k) { return k->vtype != SF_KV_FLOAT; }

@implementation SmpGui
- (Plug *)plug { return P; }

/* ---- sliders ---- */
- (int)knobOf:(SmpSlider *)s info:(SmpKnobInfo *)k {
  memset(k, 0, sizeof *k);
  return P && smp_plug_knob_info(P, SL_KNOB0 - s.what, k);
}
- (void)range:(SmpSlider *)s lo:(double *)lo hi:(double *)hi step:(double *)step {
  if (s.what == SL_LIMIT) { *lo = LIMIT_LO; *hi = LIMIT_HI; *step = 1; return; }
  if (s.what <= SL_KNOB0) {
    SmpKnobInfo k;
    if (![self knobOf:s info:&k] || !(k.hi > k.lo)) { *lo = 0; *hi = 1; *step = 0.01; return; }
    *lo = k.lo; *hi = k.hi; *step = knob_whole(&k) ? 1 : (k.hi - k.lo) / 100;
    return;
  }
  const SmpParamInfo *pi = smp_param_info(s.what);
  *lo = pi->lo; *hi = pi->hi; *step = pi->kind == SMP_K_MS ? 10 : 1;
}
- (double)get:(SmpSlider *)s {
  if (s.what <= SL_KNOB0) return smp_plug_knob_get(P, SL_KNOB0 - s.what);
  if (s.what != SL_LIMIT) return smp_plug_get_param(P, s.what);
  SmpSettings st; smp_settings_get(&st);
  return st.cache_limit_gb;
}
- (void)text:(SmpSlider *)s value:(double)v out:(char *)out cap:(int)cap {
  if (s.what <= SL_KNOB0) { SmpKnobInfo k; if ([self knobOf:s info:&k]) smp_knob_text(&k, v, out, cap); else if (cap > 0) out[0] = 0; }
  else if (s.what == SL_LIMIT) gb_text(v, out, cap); else param_text(s.what, v, out, cap);
}
- (int)parse:(SmpSlider *)s text:(const char *)t value:(double *)v {
  char *e;
  if (s.what <= SL_KNOB0) { SmpKnobInfo k; return [self knobOf:s info:&k] && smp_knob_parse(&k, t, v); }
  if (s.what != SL_LIMIT) return smp_value_parse(s.what, t, v);
  *v = strtod(t, &e);
  while (*e == ' ') e++;
  return e != t && (!*e || !strcasecmp(e, "GB")) && *v > 0;
}
/* the slider shows the value: the words as its accessible value, and the knob where the value is */
- (void)show:(SmpSlider *)s force:(BOOL)force {
  char t[64]; double v = [self get:s], lo, hi, step;
  [self text:s value:v out:t cap:sizeof t];
  NSString *w = ns(t);
  if (!force && [w isEqualToString:s.said ?: @""]) return;
  s.said = w;
  [self range:s lo:&lo hi:&hi step:&step];
  double pos = hi > lo ? floor((v - lo) / (hi - lo) * s.maxValue + 0.5) : 0;
  if (s.doubleValue != pos) s.doubleValue = pos;
  NSAccessibilityPostNotification(s, NSAccessibilityValueChangedNotification);
}
- (void)applyLimit:(double)gb {
  SmpSettings s; smp_settings_get(&s);
  s.cache_limit_gb = floor(gb * 10 + 0.5) / 10;
  [self saveSettings:&s];
  [self setText:self.used to:used_text];
}
- (void)set:(SmpSlider *)s value:(double)v {
  double lo, hi, step;
  [self range:s lo:&lo hi:&hi step:&step];
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  if (s.what <= SL_KNOB0) smp_plug_knob_set(P, SL_KNOB0 - s.what, v);
  else if (s.what == SL_LIMIT) [self applyLimit:v]; else smp_plug_set_param(P, s.what, v);
  [self show:s force:YES];
}
- (void)nudge:(SmpSlider *)s by:(double)by {
  double lo, hi, step, v;
  int kind = s.what <= SL_LIMIT ? -1 : smp_param_info(s.what)->kind;
  BOOL whole = kind == SMP_K_NOTE || kind == SMP_K_SEMI || kind == SMP_K_CENT || kind == SMP_K_COUNT || kind == SMP_K_PAN;
  if (s.what <= SL_KNOB0) { SmpKnobInfo k; whole = [self knobOf:s info:&k] && knob_whole(&k); }
  [self range:s lo:&lo hi:&hi step:&step];
  step *= fabs(by);
  if (whole && step < 1) step = 1;
  v = [self get:s] + (by < 0 ? -step : step);
  v = whole ? floor(v + 0.5) : s.what == SL_LIMIT ? floor(v * 10 + 0.5) / 10 : floor(v * 1000 + 0.5) / 1000;
  [self set:s value:v];
}
- (void)dragged:(SmpSlider *)s {                    /* the mouse moved the knob */
  double lo, hi, step, n = s.maxValue, v;
  [self range:s lo:&lo hi:&hi step:&step];
  v = lo + s.doubleValue / (n > 0 ? n : 1) * (hi - lo);
  v = s.what == SL_LIMIT ? floor(v * 10 + 0.5) / 10 : lo + floor((v - lo) / step + 0.5) * step;
  [self set:s value:v];
}
- (void)sliderMoved:(SmpSlider *)s { if (!closing && P) [self dragged:s]; }

/* Return (or a character typed) on a slider: the edit box takes its place, holding the value (or the character) */
- (void)editBegin:(SmpSlider *)s first:(NSString *)first {
  NSTextField *ed = s.ed;
  ed.stringValue = first ?: (s.said ?: @"");
  ed.frame = s.frame;
  s.editing = YES;
  ed.hidden = NO;
  [s.window makeFirstResponder:ed];
  NSText *fe = [s.window fieldEditor:NO forObject:ed];
  if (first) [fe setSelectedRange:NSMakeRange(first.length, 0)]; else [fe selectAll:nil];
  s.hidden = YES;
}
/* back to the slider: apply reads what was typed; toSlider puts the focus on the slider (Return, Escape) */
- (void)editEnd:(SmpSlider *)s apply:(BOOL)apply toSlider:(BOOL)toSlider {
  if (!s.editing) return;
  s.editing = NO;
  if (apply) {
    NSString *t = s.ed.stringValue;
    if (![t isEqualToString:s.said ?: @""]) {
      double v;
      if ([self parse:s text:t.UTF8String value:&v]) [self set:s value:v]; else beep();
    }
  }
  s.hidden = NO;
  self.last = s;
  if (toSlider) [s.window makeFirstResponder:s];
  s.ed.hidden = YES;
}

/* ---- keys ---- */
static BOOL key_is(NSEvent *e, unichar c) { NSString *k = e.charactersIgnoringModifiers; return k.length && [k characterAtIndex:0] == c; }
- (BOOL)smpKey:(NSEvent *)e from:(NSView *)v {
  NSEventModifierFlags m = e.modifierFlags;
  BOOL shift = (m & NSEventModifierFlagShift) != 0, ctrl = (m & NSEventModifierFlagControl) != 0,
       opt = (m & NSEventModifierFlagOption) != 0, cmd = (m & NSEventModifierFlagCommand) != 0;
  unichar c = e.charactersIgnoringModifiers.length ? [e.charactersIgnoringModifiers characterAtIndex:0] : 0;
  if (closing || !P || cmd) return NO;
  if ((c == '\t' || c == 0x19) && ctrl) { [self nextTab:shift || c == 0x19]; return YES; }   /* Ctrl+Tab */
  if (c == '\t' || c == 0x19) { [self tabFrom:v back:shift || c == 0x19]; return YES; }
  if ([v isKindOfClass:[SmpSlider class]]) {
    SmpSlider *s = (SmpSlider *)v; double by = shift ? 10 : (opt || ctrl) ? 0.1 : 1, lo, hi, step;
    switch (c) {
      case '\r': case 3: [self editBegin:s first:nil]; return YES;
      case NSUpArrowFunctionKey: case NSRightArrowFunctionKey: [self nudge:s by:by]; return YES;
      case NSDownArrowFunctionKey: case NSLeftArrowFunctionKey: [self nudge:s by:-by]; return YES;
      case NSPageUpFunctionKey: [self nudge:s by:10]; return YES;
      case NSPageDownFunctionKey: [self nudge:s by:-10]; return YES;
      case NSHomeFunctionKey: case NSEndFunctionKey:
        [self range:s lo:&lo hi:&hi step:&step]; [self set:s value:c == NSHomeFunctionKey ? lo : hi]; return YES;
    }
    NSString *ch = e.characters;                  /* typing starts the box */
    if (ch.length && [ch characterAtIndex:0] > ' ' && [ch characterAtIndex:0] < 0xF700 && [ch characterAtIndex:0] != 127) {
      [self editBegin:s first:ch]; return YES;
    }
    return YES;                                    /* the slider's own keys are ours */
  }
  if ([v isKindOfClass:[SmpPopUp class]]) {
    SmpPopUp *p = (SmpPopUp *)v; NSInteger n = p.numberOfItems, k = p.indexOfSelectedItem, to = -1;
    if (c == NSUpArrowFunctionKey || c == NSLeftArrowFunctionKey) to = k > 0 ? k - 1 : k;
    else if (c == NSDownArrowFunctionKey || c == NSRightArrowFunctionKey) to = k + 1 < n ? k + 1 : k;
    else if (c == NSHomeFunctionKey) to = 0;
    else if (c == NSEndFunctionKey) to = n - 1;
    else if (c == '\r' || c == 3 || c == ' ') { [p performClick:nil]; return YES; }
    if (to >= 0) {                                 /* it stops at the ends; each step applies */
      if (to != k && n > 0) { [p selectItemAtIndex:to]; [self popupChose:p]; }
      return YES;
    }
    return NO;
  }
  if ([v isKindOfClass:[SmpSegments class]]) {
    if (c == NSLeftArrowFunctionKey || c == NSRightArrowFunctionKey) {
      int k = c == NSLeftArrowFunctionKey ? 0 : 1;   /* two tabs: they stop at the ends */
      if (k != (int)self.tabs.selectedSegment) [self setTab:k];
      return YES;
    }
    return NO;
  }
  if ([v isKindOfClass:[SmpButton class]] && (c == '\r' || c == 3)) { [(SmpButton *)v performClick:nil]; return YES; }
  return NO;
}

/* keys inside text fields (the slider edit boxes, the cache folder, the read-only boxes) */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)cmd {
  (void)tv;
  SmpField *f = [control isKindOfClass:[SmpField class]] ? (SmpField *)control : nil;
  if (!f || closing || !P) return NO;
  BOOL ctrlTab = ([NSApp currentEvent].modifierFlags & NSEventModifierFlagControl) && (cmd == @selector(insertTab:) || cmd == @selector(insertBacktab:));
  if (ctrlTab) {
    if (f.forSlider) [self editEnd:f.forSlider apply:YES toSlider:NO];
    [self nextTab:cmd == @selector(insertBacktab:)];
    return YES;
  }
  if (cmd == @selector(insertTab:) || cmd == @selector(insertBacktab:)) {
    BOOL back = cmd == @selector(insertBacktab:);
    if (f.forSlider) { SmpSlider *s = f.forSlider; [self editEnd:s apply:YES toSlider:NO]; [self tabFrom:s back:back]; }
    else [self tabFrom:f back:back];
    return YES;
  }
  if (f.forSlider && (cmd == @selector(insertNewline:) || cmd == @selector(cancelOperation:))) {
    [self editEnd:f.forSlider apply:cmd == @selector(insertNewline:) toSlider:YES];
    return YES;
  }
  if (f == self.dir && cmd == @selector(insertNewline:)) { [self applyDir]; [[f currentEditor] selectAll:nil]; return YES; }
  return NO;
}
- (void)controlTextDidEndEditing:(NSNotification *)n {
  SmpField *f = n.object;
  if (closing || !P || ![f isKindOfClass:[SmpField class]]) return;
  if (f.forSlider && f.forSlider.editing) [self editEnd:f.forSlider apply:YES toSlider:NO];
  else if (f == self.dir) [self applyDir];
}

/* ---- Tab and focus ---- */
- (NSMutableArray *)orderOf:(int)pg { return pg == PG_SAMPLER ? self.order0 : pg == PG_SETTINGS ? self.order1 : self.order2; }
- (BOOL)shows:(NSView *)v { return v && !v.hidden && ![v isHiddenOrHasHiddenAncestor]; }
- (void)focus:(NSView *)v {
  if (!v || !v.window) return;
  [v.window makeFirstResponder:v];
  if ([v isKindOfClass:[NSTextField class]]) [[(NSTextField *)v currentEditor] selectAll:nil];
}
- (void)tabFrom:(NSView *)v back:(BOOL)back {
  NSArray *o = [self orderOf:page]; NSInteger n = o.count, at = [o indexOfObjectIdenticalTo:v], i, d = back ? -1 : 1;
  if (!n) return;
  if (at == NSNotFound) at = back ? n : -1;
  for (i = 1; i <= n; i++) {                       /* past either end it wraps */
    NSView *t = o[((at + d * i) % n + n) % n];
    if ([self shows:t]) { [self focus:t]; return; }
  }
}
- (void)nextTab:(BOOL)back {
  if (page == PG_SETTINGS) return;
  int k = (int)self.tabs.selectedSegment;
  (void)back;                                      /* two tabs: next and previous are the same one */
  [self setTab:k ? 0 : 1];
}

/* ---- text ---- */
- (void)setText:(NSTextField *)f to:(const char *)s {   /* only when it changed, so VoiceOver isn't told twice */
  NSString *w = ns(s);
  if ([f.stringValue isEqualToString:w]) return;
  if (f.currentEditor && f.isEditable) return;     /* the user is typing in it */
  f.stringValue = w;
  NSAccessibilityPostNotification(f, NSAccessibilityValueChangedNotification);
}
- (void)announce:(NSString *)msg {
  NSView *v = self.view;
  if (!msg.length || !v.window) return;
  NSAccessibilityPostNotificationWithUserInfo(v.window, NSAccessibilityAnnouncementRequestedNotification,
    @{NSAccessibilityAnnouncementKey: msg, NSAccessibilityPriorityKey: @(NSAccessibilityPriorityHigh)});
}
- (void)sayNote:(const char *)msg {
  snprintf(note, sizeof note, "%s", msg);
  note_until = GetTickCount64() + NOTE_MS;
}

/* ---- the Settings page ---- */
- (void)rescanUsed {
  SmpSettings s; char why[600], lim[32]; uint64_t b;
  smp_settings_get(&s);
  if (!smp_cache_ok(why, sizeof why)) { snprintf(used_text, sizeof used_text, "Not working: %s", why); return; }
  b = smp_cache_used_bytes();
  gb_text(s.cache_limit_gb, lim, sizeof lim);
  if (b < (1ull << 30)) snprintf(used_text, sizeof used_text, "%.0f MB of %s", b / 1048576.0, lim);
  else snprintf(used_text, sizeof used_text, "%.1f GB of %s", b / 1073741824.0, lim);
}
- (void)findFFmpeg:(int)force {
  SmpSettings s; smp_settings_get(&s);
  if (!force && ff_searched && !strcmp(ff_for, s.ffmpeg_path)) return;
  if (!smp_ffmpeg_find(ff_text, sizeof ff_text)) snprintf(ff_text, sizeof ff_text, "Not found");
  snprintf(ff_for, sizeof ff_for, "%s", s.ffmpeg_path);
  ff_searched = 1;
}
static int rate_index(int rate) { const int *r; int n = smp_raw_rates(&r), i; for (i = 0; i < n; i++) if (r[i] == rate) return i; return 0; }
static void sel(NSPopUpButton *p, NSInteger k) { if (p.indexOfSelectedItem != k && k < p.numberOfItems) [p selectItemAtIndex:k]; }
- (void)showSettings {
  SmpSettings s; smp_settings_get(&s);
  if (!self.dir.currentEditor) { [self setText:self.dir to:s.cache_dir]; snprintf(dir_shown, sizeof dir_shown, "%s", s.cache_dir); }
  if (!self.limit.editing) [self show:self.limit force:NO];
  [self setText:self.used to:used_text];
  sel(self.stretcher, s.stretcher);
  sel(self.keeptone, s.keep_tone);
  sel(self.rawrate, rate_index(s.raw_rate));
  sel(self.rawch, s.raw_channels == 1 ? 0 : 1);
  sel(self.rawfmt, s.raw_format);
  sel(self.rawend, s.raw_endian);
  [self setText:self.ffmpeg to:ff_text];
}
- (void)saveSettings:(const SmpSettings *)s { if (smp_settings_set(s)) beep(); [self rescanUsed]; }
- (int)applyDir {
  SmpSettings s; const char *t = self.dir.stringValue.UTF8String ?: "";
  if (!strcmp(t, dir_shown)) return 1;
  smp_settings_get(&s);
  if (!t[0] || strlen(t) >= sizeof s.cache_dir) { beep(); [self setText:self.dir to:dir_shown]; return 0; }
  strcpy(s.cache_dir, t);
  [self saveSettings:&s];
  snprintf(dir_shown, sizeof dir_shown, "%s", t);
  [self setText:self.used to:used_text];
  return 1;
}
- (void)chooseSetting:(SmpPopUp *)p {
  SmpSettings s; const int *r; int n = smp_raw_rates(&r), k = (int)p.indexOfSelectedItem;
  if (k < 0) return;
  smp_settings_get(&s);
  if (p == self.stretcher) s.stretcher = k;
  else if (p == self.keeptone) s.keep_tone = k;
  else if (p == self.rawrate) s.raw_rate = r[k < n ? k : n - 1];
  else if (p == self.rawch) s.raw_channels = k ? 2 : 1;
  else if (p == self.rawfmt) s.raw_format = k;
  else if (p == self.rawend) s.raw_endian = k;
  if (smp_settings_set(&s)) beep();
  [self showSettings];
  if (p == self.rawrate || p == self.rawch || p == self.rawfmt || p == self.rawend) {
    char path[1100];
    if (smp_plug_sample_path(P, path, sizeof path) == 1 && smp_is_raw_name(path))
      [self sayNote:"Raw settings apply the next time a raw file loads"];
  }
}
- (void)clearCache { smp_cache_trim(0); [self rescanUsed]; [self setText:self.used to:used_text]; }

/* ---- dialogs: the window may close while one is open (the host closes the plugin) ---- */
- (void)modalBegin { modal++; }
- (BOOL)modalEnd { modal--; return closing; }          /* YES: the window closed meanwhile; touch nothing */
- (NSString *)runPanelDirs:(BOOL)dirs title:(NSString *)title from:(NSString *)start {
  NSOpenPanel *op = [NSOpenPanel openPanel];
  op.canChooseFiles = !dirs; op.canChooseDirectories = dirs; op.canCreateDirectories = dirs;
  op.allowsMultipleSelection = NO; op.message = title; op.title = title;
  if (start.length) op.directoryURL = [NSURL fileURLWithPath:start isDirectory:YES];
  [self modalBegin];
  g_panels++;
  NSModalResponse r = [op runModal];
  g_panels--;
  if ([self modalEnd]) return nil;
  [self.view.window makeKeyAndOrderFront:nil];   /* back to the plugin's window (a host may give the focus elsewhere) */
  return r == NSModalResponseOK ? op.URL.path : nil;
}
- (void)browseFolder {
  NSString *p = [self runPanelDirs:YES title:@"Cache folder" from:ns(dir_shown)];
  if (closing) return;
  if (p) {
    SmpSettings s; smp_settings_get(&s);
    if (strlen(p.UTF8String) < sizeof s.cache_dir) { strcpy(s.cache_dir, p.UTF8String); [self saveSettings:&s]; [self showSettings]; }
    else beep();
  }
  [self focus:self.dirbrowse];
}
- (void)browseFFmpeg {
  char found[1100]; NSString *start = nil;
  if (smp_ffmpeg_find(found, sizeof found)) start = [ns(found) stringByDeletingLastPathComponent];
  NSString *p = [self runPanelDirs:NO title:@"Find ffmpeg" from:start];
  if (closing) return;
  if (p) {
    SmpSettings s; smp_settings_get(&s);
    if (strlen(p.UTF8String) < sizeof s.ffmpeg_path) {
      strcpy(s.ffmpeg_path, p.UTF8String);
      if (smp_settings_set(&s)) beep();
      [self findFFmpeg:1];
      [self showSettings];
    } else beep();
  }
  [self focus:self.ffbrowse];
}

/* ---- the sample and the SoundFont ---- */
- (void)showStatus {
  char t[1900];
  if (note[0] && GetTickCount64() < note_until) snprintf(t, sizeof t, "%s", note);
  else { note[0] = 0; smp_plug_status_text(P, t, sizeof t); }
  [self setText:self.status to:t];
  [self setText:self.sfstatus to:t];
}
- (void)showSample { char t[1500]; smp_plug_sample_text(P, t, sizeof t); [self setText:self.sample to:t]; }

- (void)sfCfg:(SfCfg *)c {
  SmpSynth *s = smp_plug_synth(P);
  int n = s ? smp_synth_script_write(s, NULL, 0) : 0, i;
  char *t = n > 0 ? malloc((size_t)n + 1) : NULL, *p;
  memset(c, 0, sizeof *c);
  for (i = 0; i < NCH; i++) c->pin[i] = -1;
  if (!t) return;
  smp_synth_script_write(s, t, n + 1);
  for (p = t; p && *p; ) {
    char *e = strchr(p, '\n'), w[32] = "", a[32] = ""; int x, y, z;
    if (e) *e = 0;
    if (sscanf(p, "%31s %31s", w, a) >= 1) {
      if (!strcmp(w, "sf_mode")) c->one = !strcmp(a, "one");
      else if (!strcmp(w, "sf_preset") && sscanf(p + 9, "%d %d", &x, &y) == 2) { c->bank = x; c->program = y; }
      else if (!strcmp(w, "sf_channel")) c->channel = strcmp(a, "all") ? atoi(a) : 0;
      else if (!strcmp(w, "sf_channel_preset") && sscanf(p + 17, "%d %d %d", &x, &y, &z) == 3 && x >= 1 && x <= NCH)
        c->pin[x - 1] = y * 128 + z;
      else if (!strcmp(w, "sf_output") && sscanf(p + 9, "%d %d", &x, &y) == 2 && x >= 1 && x <= NCH) c->out[x - 1] = y - 1;
    }
    p = e ? e + 1 : NULL;
  }
  free(t);
}
- (SmpFont *)font { SmpSynth *s = smp_plug_synth(P); return s ? smp_synth_font(s) : NULL; }

/* the preset lists: Preset's, and each channel's after its first entry; built again only when the font changes */
- (void)sfLists {
  SmpFont *f = [self font];
  char key[1200], name[200], t[300]; int n = f ? smp_sf_preset_count(f) : 0, i, c, b, p;
  smp_plug_font_text(P, key, sizeof key);
  if (list_key[0] && (const void *)f == list_font && !strcmp(key, list_key)) return;
  list_font = f; snprintf(list_key, sizeof list_key, "%s", key);
  NSMutableArray *titles = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
  for (i = 0; i < n; i++) {
    if (!smp_sf_preset_info(f, i, &b, &p, name, sizeof name)) name[0] = 0, b = p = 0;
    snprintf(t, sizeof t, "%s, bank %d program %d", name, b, p + 1);
    [titles addObject:ns(t)];
  }
  lacks_preset[0] = 0;
  [self.sfpreset removeAllItems];
  for (NSString *s in titles) [self.sfpreset.menu addItemWithTitle:s action:nil keyEquivalent:@""];
  for (c = 0; c < NCH; c++) {
    SmpPopUp *pp = self.chan[c];
    [pp removeAllItems];
    [pp.menu addItemWithTitle:@"From the song" action:nil keyEquivalent:@""];
    for (NSString *s in titles) [pp.menu addItemWithTitle:s action:nil keyEquivalent:@""];
    from_song[c][0] = 0; lacks_chan[c][0] = 0;
  }
}
/* A chosen preset the font doesn't have: an entry after the presets (at base) saying so, selected. */
static int lacks_entry(NSPopUpButton *c, char *have, int cap, int base, const char *want) {
  if (strcmp(have, want ? want : "")) {
    if (have[0] && base < c.numberOfItems) [c removeItemAtIndex:base];
    if (want) [c.menu addItemWithTitle:ns(want) action:nil keyEquivalent:@""];
    snprintf(have, (size_t)cap, "%s", want ? want : "");
  }
  return want ? base : -1;
}
- (int)sfIndexBank:(int)bank program:(int)program { SmpFont *f = [self font]; return f ? smp_sf_find_preset(f, bank, program) : -1; }

- (BOOL)modeHides:(NSView *)v {
  for (int i = nknobs; i < NKNOB; i++) if (v == self.knob[i] || v == self.knobLab[i]) return YES;
  return sf_one ? [self.modeAll indexOfObjectIdenticalTo:v] != NSNotFound : [self.modeOne indexOfObjectIdenticalTo:v] != NSNotFound;
}
- (void)sfLayout {
  NSView *fr = owner_of(self.view.window.firstResponder);
  for (NSView *v in self.modeOne) v.hidden = !sf_one;
  for (NSView *v in self.modeAll) v.hidden = sf_one;
  for (int i = 0; i < NKNOB; i++) {
    SmpSlider *s = self.knob[i]; BOOL sh = i < nknobs;
    ((NSView *)self.knobLab[i]).hidden = !sh;
    if (!s.editing) s.hidden = !sh;
  }
  if (page == PG_FONT && fr && [self modeHides:fr]) [self focus:self.sfmode];
}
static void move_labelled(NSView *lab, NSView *c, NSView *ed, CGFloat x, CGFloat y) {
  NSRect r = lab.frame; CGFloat lw = r.size.width;
  [lab setFrameOrigin:NSMakePoint(x, y + 4)];
  [c setFrameOrigin:NSMakePoint(x + lw + 4, y)];
  if (ed) [ed setFrameOrigin:NSMakePoint(x + lw + 4, y)];
}
static void knob_place(int slot, int ty, int *x, int *y) { *x = slot % 2 == 0 ? 384 : 8; *y = ty + ((slot + 1) / 2) * ROW; }
- (void)knobLayout {
  int i, x, y;
  for (i = 0; i < nknobs; i++) { knob_place(i, transpose_y, &x, &y); SmpSlider *s = self.knob[i]; move_labelled(self.knobLab[i], s, s.ed, x, y); }
  knob_place(nknobs, transpose_y, &x, &y);
  move_labelled(self.fxLab, self.valOf[@(SMP_P_SF_EFFECTS)], nil, x, y);
  y += ROW;
  move_labelled(self.sfstatusLab, self.sfstatus, nil, 8, y);
  [self.sfsettings setFrameOrigin:NSMakePoint(132, y + ROW)];
}
- (void)sfKnobs {
  char key[2200]; int n = smp_plug_knob_count(P), i, at;
  SmpKnobInfo k[NKNOB];
  if (n > NKNOB) n = NKNOB;
  at = snprintf(key, sizeof key, "%p|%d", (void *)[self font], n);
  for (i = 0; i < n; i++) {
    if (!smp_plug_knob_info(P, i, &k[i])) memset(&k[i], 0, sizeof k[i]);
    if (at < (int)sizeof key) at += snprintf(key + at, sizeof key - (size_t)at, "|%s %g %g %d", k[i].label, k[i].lo, k[i].hi, k[i].vtype);
  }
  if (strcmp(key, knob_key)) {
    NSView *f = owner_of(self.view.window.firstResponder); BOOL lost = NO;
    snprintf(knob_key, sizeof knob_key, "%s", key);
    for (i = n; i < NKNOB; i++) {
      SmpSlider *s = self.knob[i];
      if (s.editing) [self editEnd:s apply:NO toSlider:NO];
      if (f && (f == s || f == s.ed)) lost = YES;
    }
    nknobs = n;
    for (i = 0; i < n; i++) {
      SmpSlider *s = self.knob[i];
      double steps = knob_whole(&k[i]) ? k[i].hi - k[i].lo : 100;
      NSString *lab = ns(k[i].label);
      ((NSTextField *)self.knobLab[i]).stringValue = lab;
      [(NSTextField *)self.knobLab[i] sizeToFit];
      s.accessibilityLabel = lab; s.ed.accessibilityLabel = lab;
      s.maxValue = steps > 1000 ? 1000 : steps < 1 ? 1 : floor(steps + 0.5);
      s.said = nil;
    }
    [self knobLayout];
    if (page == PG_FONT) [self sfLayout];
    if (lost) [self focus:self.valOf[@(SMP_P_SF_TRANSPOSE)]];
  }
  for (i = 0; i < n; i++) { SmpSlider *s = self.knob[i]; if (!s.editing) [self show:s force:NO]; }
}

- (void)sfShow {
  char t[1500], name[200], lacks[200]; SfCfg c; SmpFont *f; int i, b, p, n, k;
  smp_plug_font_text(P, t, sizeof t); [self setText:self.sffile to:t];
  [self sfLists];
  [self sfCfg:&c];
  f = [self font];
  n = f ? smp_sf_preset_count(f) : 0;
  if ((BOOL)(c.one != 0) != sf_one) { sf_one = c.one != 0; [self sfLayout]; }
  [self sfKnobs];
  sel(self.sfmode, c.one ? 1 : 0);
  k = [self sfIndexBank:c.bank program:c.program];
  snprintf(lacks, sizeof lacks, "Bank %d program %d (not in this font)", c.bank, c.program + 1);
  if (!self.sfpreset.menuOpen) {
    if (k < 0) k = lacks_entry(self.sfpreset, lacks_preset, sizeof lacks_preset, n, f ? lacks : NULL);
    else lacks_entry(self.sfpreset, lacks_preset, sizeof lacks_preset, n, NULL);
    if (k >= 0) sel(self.sfpreset, k);
  }
  sel(self.sfchannel, c.channel);
  for (i = 0; i < NCH; i++) {
    SmpPopUp *pp = self.chan[i];
    if (pp.menuOpen) continue;                   /* never while its list is open (it would jump) */
    k = c.pin[i] >= 0 ? [self sfIndexBank:c.pin[i] / 128 program:c.pin[i] % 128] : -1;
    if (f && smp_plug_sf_channel_now(P, i + 1, &b, &p)) {
      int at = smp_sf_find_preset(f, b, p);
      if (at < 0 || !smp_sf_preset_info(f, at, NULL, NULL, name, sizeof name)) snprintf(name, sizeof name, "bank %d program %d", b, p + 1);
      snprintf(t, sizeof t, "From the song (now %s)", name);
    } else snprintf(t, sizeof t, "From the song");
    if (strcmp(t, from_song[i]) && pp.numberOfItems > 0) {
      snprintf(from_song[i], sizeof from_song[i], "%s", t);
      [pp itemAtIndex:0].title = ns(t);
      if (pp.indexOfSelectedItem == 0) [pp synchronizeTitleAndSelectedItem];
    }
    if (c.pin[i] >= 0 && k < 0 && f) {
      snprintf(lacks, sizeof lacks, "Bank %d program %d (not in this font)", c.pin[i] / 128, c.pin[i] % 128 + 1);
      k = lacks_entry(pp, lacks_chan[i], sizeof lacks_chan[i], n + 1, lacks) - 1;
    } else lacks_entry(pp, lacks_chan[i], sizeof lacks_chan[i], n + 1, NULL);
    NSInteger want = c.pin[i] >= 0 ? (k >= 0 ? k + 1 : -1) : 0;
    if (want >= 0) sel(pp, want);
    sel(self.outp[i], c.out[i]);
  }
}

/* a SoundFont pop-up chose entry k: one state line to the player (one it won't take beeps) */
- (void)sfChoose:(SmpPopUp *)pp {
  char line[200] = ""; int b = 0, p = 0, k = (int)pp.indexOfSelectedItem; SmpFont *f = [self font];
  int n = f ? smp_sf_preset_count(f) : 0;
  NSUInteger ci = [self.chan indexOfObjectIdenticalTo:pp], oi = [self.outp indexOfObjectIdenticalTo:pp];
  if (k < 0) return;
  if ((pp == self.sfpreset && k >= n) || (ci != NSNotFound && k > n)) { [self sfShow]; return; }
  if (pp == self.sfmode) snprintf(line, sizeof line, "sf_mode %s", k ? "one" : "all");
  else if (pp == self.sfpreset) { if (f && smp_sf_preset_info(f, k, &b, &p, NULL, 0)) snprintf(line, sizeof line, "sf_preset %d %d", b, p); }
  else if (pp == self.sfchannel) { if (k) snprintf(line, sizeof line, "sf_channel %d", k); else snprintf(line, sizeof line, "sf_channel all"); }
  else if (ci != NSNotFound) {
    if (!k) snprintf(line, sizeof line, "sf_channel_preset %d song", (int)ci + 1);
    else if (f && smp_sf_preset_info(f, k - 1, &b, &p, NULL, 0)) snprintf(line, sizeof line, "sf_channel_preset %d %d %d", (int)ci + 1, b, p);
  } else if (oi != NSNotFound) snprintf(line, sizeof line, "sf_output %d %d", (int)oi + 1, k + 1);
  if (!line[0] || !smp_plug_sf_set(P, line)) beep();
  [self sfShow];
}

- (void)showParam:(int)pid force:(BOOL)force {
  id c = self.valOf[@(pid)];
  if (is_choice(pid)) { sel(c, (NSInteger)floor(smp_plug_get_param(P, pid) + 0.5)); return; }
  SmpSlider *s = c;
  if (!s.editing) [self show:s force:force];
}

/* ---- refresh: the 250 ms timer, a finished load, a page switch ---- */
- (void)refresh {
  if (closing || !P) return;
  smp_plug_poll_load(P);
  if (P->load_failed != fails_seen) {             /* a load that failed: Status for a while, a beep, announced */
    fails_seen = P->load_failed;
    [self sayNote:P->load_err]; beep(); [self announce:ns(P->load_err)];
  }
  if (P->load_notes != notes_seen) { notes_seen = P->load_notes; [self sayNote:P->load_note]; }
  if (P->e) smp_engine_collect(P->e);
  if (page != PG_SETTINGS && page != (smp_plug_plays(P) ? PG_FONT : PG_SAMPLER)) {
    [self showPage:smp_plug_plays(P) ? PG_FONT : PG_SAMPLER to:nil];
    return;
  }
  [self showSample];
  if (page != PG_SETTINGS) [self fileList:page == PG_FONT rescan:NO];
  for (int id = 0; id < SMP_NALLPARAMS; id++) [self showParam:id force:NO];
  [self showStatus];
  if (page == PG_FONT) [self sfShow];
  if (page == PG_SETTINGS) [self showSettings];
}
- (void)tick:(NSTimer *)t { (void)t; if (!modal) [self refresh]; }

/* ---- files ---- */
static NSArray *font_exts(void) {
  return @[@"sf2", @"sf3", @"sf4", @"sf2pack", @"sbk", @"dls", @"gig", @"sfz", @"dspreset", @"dslibrary", @"exs"];
}
static BOOL font_name(NSString *n) { return [font_exts() containsObject:n.pathExtension.lowercaseString]; }
static BOOL audio_name(NSString *n) {
  const wchar_t **ext; int k = smp_audio_extensions(&ext);
  NSString *e = [@"." stringByAppendingString:n.pathExtension.lowercaseString];
  if (!n.pathExtension.length) return NO;
  for (int i = 0; i < k; i++) {
    char u[32];
    if (WideCharToMultiByte(CP_UTF8, 0, ext[i], -1, u, sizeof u, NULL, NULL) > 0 && [e caseInsensitiveCompare:ns(u)] == NSOrderedSame) return YES;
  }
  return NO;
}
- (void)loadPath:(NSString *)path how:(int)how {
  char err[700], msg[1900]; const char *u = path.fileSystemRepresentation;
  BOOL sf = how == LOAD_DROPPED ? font_name(path) : how == LOAD_FONT;
  int ok = sf ? smp_plug_load_font_start(P, u, err, sizeof err) : smp_plug_load_start(P, u, how == LOAD_DROPPED, err, sizeof err);
  if (!ok) {
    smp_plug_cant_load(P, u, err, msg, sizeof msg);
    [self sayNote:msg]; beep(); [self announce:ns(msg)];
  } else note[0] = 0;
  [self refresh];
}
/* the File pop-up: the files of its kind in the loaded file's folder, alphabetical (ignoring case), the loaded one
   selected; with nothing loaded, "No file loaded" or "<name> (missing)" */
- (void)fileList:(int)kind rescan:(BOOL)rescan {
  SmpPopUp *c = kind ? self.file1 : self.file0;
  char path[1100], key[1110]; int rc;
  if (!c) return;
  if (c.menuOpen) { file_key[kind][0] = 0; return; }
  rc = kind ? smp_plug_font_path(P, path, sizeof path) : smp_plug_sample_path(P, path, sizeof path);
  snprintf(key, sizeof key, "%d%s", rc, path);
  if (!rescan && !strcmp(key, file_key[kind])) return;
  snprintf(file_key[kind], sizeof file_key[kind], "%s", key);
  file_dir[kind][0] = 0;
  NSString *full = rc ? [ns(path) stringByReplacingOccurrencesOfString:@"\\" withString:@"/"] : nil;
  NSString *name = full.lastPathComponent, *dir = full.stringByDeletingLastPathComponent;
  NSMutableArray *names = [NSMutableArray array];
  NSInteger at = -1;
  if (rc == 1 && dir.length) {
    snprintf(file_dir[kind], sizeof file_dir[kind], "%s", dir.fileSystemRepresentation);
    for (NSString *n in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]) {
      BOOL isDir = NO;
      [[NSFileManager defaultManager] fileExistsAtPath:[dir stringByAppendingPathComponent:n] isDirectory:&isDir];
      if (isDir && ![n.pathExtension.lowercaseString isEqualToString:@"dslibrary"]) continue;
      if (kind ? font_name(n) : audio_name(n)) [names addObject:n];
      else if ([n caseInsensitiveCompare:name] == NSOrderedSame) [names addObject:n];
    }
    [names sortUsingSelector:@selector(caseInsensitiveCompare:)];
    for (NSUInteger i = 0; i < names.count; i++) if ([names[i] caseInsensitiveCompare:name] == NSOrderedSame) at = (NSInteger)i;
  }
  if (rc != 1 || at < 0) {
    [names removeAllObjects];
    at = 0;
    [names addObject:rc ? (rc == 2 ? [name stringByAppendingString:@" (missing)"] : name) : @"No file loaded"];
  }
  NSArray *now = c.itemTitles;
  if (![now isEqualToArray:names]) { [c removeAllItems]; for (NSString *n in names) [c.menu addItemWithTitle:n action:nil keyEquivalent:@""]; }
  file_live[kind] = rc == 1 && names.count > 0 && at >= 0 && file_dir[kind][0];
  if (names.count) sel(c, at);
}
- (void)fileChosen:(int)kind {
  SmpPopUp *c = kind ? self.file1 : self.file0; NSInteger k = c.indexOfSelectedItem;
  if (k < 0 || !file_live[kind]) { [self fileList:kind rescan:YES]; return; }
  NSString *p = [ns(file_dir[kind]) stringByAppendingPathComponent:c.titleOfSelectedItem];
  [self loadPath:p how:kind ? LOAD_FONT : LOAD_SAMPLE];
  [self fileList:kind rescan:YES];
}
- (void)browseFile:(int)kind {
  char path[1100]; NSString *start = nil;
  if ((kind ? smp_plug_font_path(P, path, sizeof path) : smp_plug_sample_path(P, path, sizeof path)))
    start = [[ns(path) stringByReplacingOccurrencesOfString:@"\\" withString:@"/"] stringByDeletingLastPathComponent];
  NSString *p = [self runPanelDirs:NO title:kind ? @"Load a SoundFont" : @"Load a sample" from:start];
  if (closing) return;
  if (p) [self loadPath:p how:kind ? LOAD_FONT : LOAD_SAMPLE];
  [self focus:kind ? self.sfbrowse : self.browse];
}

/* ---- pages ---- */
- (NSView *)pageView:(int)pg { return pg == PG_SAMPLER ? self.pageView0 : pg == PG_SETTINGS ? self.pageView1 : self.pageView2; }
- (void)showPage:(int)pg to:(NSView *)to {
  NSView *fr = owner_of(self.view.window.firstResponder);
  if (pg == PG_SETTINGS) { [self rescanUsed]; [self findFFmpeg:0]; [self showSettings]; }
  if (pg == PG_FONT) { SfCfg c; [self sfCfg:&c]; sf_one = c.one != 0; [self sfLayout]; }
  page = pg;
  if (pg != PG_SETTINGS) {
    setting_tab = YES;
    if (self.tabs.selectedSegment != (pg == PG_FONT ? 1 : 0)) self.tabs.selectedSegment = pg == PG_FONT ? 1 : 0;
    setting_tab = NO;
  }
  [self pageView:pg].hidden = NO;
  self.tabs.hidden = pg == PG_SETTINGS;
  if (to) [self focus:to];
  else if (fr && [fr isDescendantOf:self.view] && ([fr isDescendantOf:[self pageView:(pg + 1) % NPAGES]] ||
                                                  [fr isDescendantOf:[self pageView:(pg + 2) % NPAGES]] ||
                                                  (pg == PG_SETTINGS && fr == self.tabs)))
    [self focus:[self orderOf:pg].firstObject];
  for (int q = 0; q < NPAGES; q++) if (q != pg) [self pageView:q].hidden = YES;
  if (pg != PG_SETTINGS) [self refresh];
}
- (void)setTab:(int)which { smp_plug_set_plays(P, which); [self showPage:which ? PG_FONT : PG_SAMPLER to:self.tabs]; }
- (void)tabChosen:(SmpSegments *)s { if (!setting_tab && !closing && P) [self setTab:(int)s.selectedSegment]; }

/* ---- actions ---- */
- (void)popupChose:(SmpPopUp *)p {
  if (closing || !P) return;
  if (p == self.file0 || p == self.file1) { [self fileChosen:p == self.file1]; return; }
  if (p == self.stretcher || p == self.keeptone || p == self.rawrate || p == self.rawch || p == self.rawfmt || p == self.rawend) { [self chooseSetting:p]; return; }
  if (p == self.sfmode || p == self.sfpreset || p == self.sfchannel || [self.chan indexOfObjectIdenticalTo:p] != NSNotFound ||
      [self.outp indexOfObjectIdenticalTo:p] != NSNotFound) { [self sfChoose:p]; return; }
  for (NSNumber *k in self.valOf) if (self.valOf[k] == p) { if (p.indexOfSelectedItem >= 0) smp_plug_set_param(P, k.intValue, (double)p.indexOfSelectedItem); return; }
}
- (void)clicked:(SmpButton *)b {
  if (closing || !P) return;
  if (b == self.browse) [self browseFile:0];
  else if (b == self.sfbrowse) [self browseFile:1];
  else if (b == self.settings || b == self.sfsettings) [self showPage:PG_SETTINGS to:nil];
  else if (b == self.back) { int m = smp_plug_plays(P) ? PG_FONT : PG_SAMPLER; [self showPage:m to:m == PG_FONT ? self.sfsettings : self.settings]; }
  else if (b == self.dirbrowse) [self browseFolder];
  else if (b == self.ffbrowse) [self browseFFmpeg];
  else if (b == self.clear) [self clearCache];
}
- (void)focusedFile:(SmpPopUp *)p { [self fileList:p == self.file1 rescan:YES]; }   /* new files show */

/* ================================================================ building the window */
- (NSTextField *)label:(NSString *)t page:(int)pg x:(CGFloat)x y:(CGFloat)y w:(CGFloat)w {
  NSTextField *l = [NSTextField labelWithString:t];
  l.frame = NSMakeRect(x, y + 4, w, 20);
  l.font = [NSFont systemFontOfSize:12];
  l.accessibilityElement = NO;                   /* the control carries the name */
  l.cell.accessibilityElement = NO;
  [[self pageView:pg] addSubview:l];
  return l;
}
- (void)addToOrder:(NSView *)v page:(int)pg { [[self orderOf:pg] addObject:v]; }
- (SmpPopUp *)popup:(NSString *)lab page:(int)pg x:(CGFloat)x y:(CGFloat)y lw:(CGFloat)lw cw:(CGFloat)cw items:(NSArray *)items {
  if (lab.length) [self label:lab page:pg x:x y:y w:lw];
  SmpPopUp *p = [[SmpPopUp alloc] initWithFrame:NSMakeRect(x + lw + 4, y, cw, 26) pullsDown:NO];
  p.gui = self; p.font = [NSFont systemFontOfSize:12];
  p.accessibilityLabel = lab;
  p.menu.delegate = p;
  [p addItemsWithTitles:items ?: @[]];
  p.target = self; p.action = @selector(popupChose:);
  [[self pageView:pg] addSubview:p];
  [self addToOrder:p page:pg];
  return p;
}
- (SmpButton *)button:(NSString *)t page:(int)pg x:(CGFloat)x y:(CGFloat)y w:(CGFloat)w {
  SmpButton *b = [SmpButton buttonWithTitle:t target:self action:@selector(clicked:)];
  b.gui = self; b.frame = NSMakeRect(x, y, w, 28); b.font = [NSFont systemFontOfSize:12];
  [[self pageView:pg] addSubview:b];
  [self addToOrder:b page:pg];
  return b;
}
- (SmpField *)field:(NSString *)lab page:(int)pg x:(CGFloat)x y:(CGFloat)y lw:(CGFloat)lw cw:(CGFloat)cw editable:(BOOL)editable {
  [self label:lab page:pg x:x y:y w:lw];
  SmpField *f = [[SmpField alloc] initWithFrame:NSMakeRect(x + lw + 4, y, cw, 24)];
  f.gui = self; f.font = [NSFont systemFontOfSize:12];
  f.editable = editable; f.selectable = YES; f.bezeled = YES; f.drawsBackground = YES;
  f.lineBreakMode = NSLineBreakByTruncatingTail; f.usesSingleLineMode = YES;
  f.accessibilityLabel = lab;
  f.delegate = self;
  [[self pageView:pg] addSubview:f];
  [self addToOrder:f page:pg];
  return f;
}
- (SmpSlider *)slider:(NSString *)lab what:(int)what page:(int)pg x:(CGFloat)x y:(CGFloat)y lw:(CGFloat)lw cw:(CGFloat)cw {
  double lo, hi, step, n;
  [self label:lab page:pg x:x y:y w:lw];
  SmpSlider *s = [[SmpSlider alloc] initWithFrame:NSMakeRect(x + lw + 4, y, cw, 24)];
  s.gui = self; s.what = what;
  s.accessibilityLabel = lab;
  s.minValue = 0;
  [self range:s lo:&lo hi:&hi step:&step];
  n = floor((hi - lo) / step + 0.5);
  s.maxValue = n > 1000 ? 1000 : n < 1 ? 1 : n;
  s.continuous = YES; s.target = self; s.action = @selector(sliderMoved:);
  SmpField *ed = [[SmpField alloc] initWithFrame:s.frame];
  ed.gui = self; ed.forSlider = s; ed.editable = YES; ed.selectable = YES; ed.bezeled = YES;
  ed.font = [NSFont systemFontOfSize:12]; ed.usesSingleLineMode = YES; ed.hidden = YES; ed.delegate = self;
  ed.accessibilityLabel = lab;
  s.ed = ed;
  [[self pageView:pg] addSubview:s];
  [[self pageView:pg] addSubview:ed];
  [self addToOrder:s page:pg];
  [self.sliders addObject:s];
  [self show:s force:YES];
  return s;
}
- (NSView *)paramBox:(int)id page:(int)pg x:(CGFloat)x y:(CGFloat)y lw:(CGFloat)lw cw:(CGFloat)cw {
  NSString *lab = ns(smp_param_label[id]);
  NSView *c;
  if (is_choice(id)) {
    int n = (int)(smp_param_info(id)->hi + 0.5);
    NSMutableArray *items = [NSMutableArray array];
    for (int k = 0; k <= n; k++) { char t[64]; param_text(id, k, t, sizeof t); [items addObject:ns(t)]; }
    c = [self popup:lab page:pg x:x y:y lw:lw cw:cw items:items];
  } else c = [self slider:lab what:id page:pg x:x y:y lw:lw cw:cw];
  self.valOf[@(id)] = c;
  return c;
}
- (NSArray *)subviewsSince:(NSUInteger)k page:(int)pg {
  NSArray *a = [self pageView:pg].subviews;
  return k < a.count ? [a subarrayWithRange:NSMakeRange(k, a.count - k)] : @[];
}

- (void)build {
  int id, k, y; const CGFloat W = W_WIDTH;
  self.view = [[SmpView alloc] initWithFrame:NSMakeRect(0, 0, W_WIDTH, W_HEIGHT)];
  self.view.gui = self;
  [self.view registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
  self.order0 = [NSMutableArray array]; self.order1 = [NSMutableArray array]; self.order2 = [NSMutableArray array];
  self.sliders = [NSMutableArray array]; self.valOf = [NSMutableDictionary dictionary];
  self.chan = [NSMutableArray array]; self.outp = [NSMutableArray array];
  self.modeOne = [NSMutableArray array]; self.modeAll = [NSMutableArray array];
  self.knob = [NSMutableArray array]; self.knobLab = [NSMutableArray array];
  for (int pg = 0; pg < NPAGES; pg++) {
    SmpView *v = [[SmpView alloc] initWithFrame:NSMakeRect(0, 0, W_WIDTH, W_HEIGHT)];
    v.gui = self; v.hidden = YES;
    [self.view addSubview:v];
    if (pg == 0) self.pageView0 = v; else if (pg == 1) self.pageView1 = v; else self.pageView2 = v;
  }

  /* the Player tabs: first on both main pages */
  y = 8;
  self.tabs = [SmpSegments segmentedControlWithLabels:@[@"Sampler", @"SoundFont"] trackingMode:NSSegmentSwitchTrackingSelectOne
                                               target:self action:@selector(tabChosen:)];
  self.tabs.gui = self; self.tabs.frame = NSMakeRect(8, y, 300, 26);
  self.tabs.accessibilityLabel = @"Player";
  [self.view addSubview:self.tabs positioned:NSWindowBelow relativeTo:nil];   /* first, as VoiceOver reads them */
  [self.order0 addObject:self.tabs]; [self.order2 addObject:self.tabs];

  /* the Sampler page, in Tab order */
  y += ROW;
  self.sample = [self field:@"Sample" page:PG_SAMPLER x:8 y:y lw:120 cw:W - 140 editable:NO];
  y += ROW;
  self.browse = [self button:@"Browse" page:PG_SAMPLER x:132 y:y w:100];
  self.file0 = [self popup:@"File" page:PG_SAMPLER x:240 y:y lw:40 cw:470 items:nil];
  y += ROW;
  [self paramBox:SMP_P_KEEP_LENGTH page:PG_SAMPLER x:8 y:y lw:120 cw:130];
  [self paramBox:SMP_P_MODE page:PG_SAMPLER x:384 y:y lw:160 cw:130];
  y += ROW;
  for (id = 0, k = 0; id < SMP_NPARAMS; id++) {
    if (id == SMP_P_KEEP_LENGTH || id == SMP_P_MODE) continue;
    [self paramBox:id page:PG_SAMPLER x:k % 2 ? 384 : 8 y:y + (k / 2) * ROW lw:k % 2 ? 160 : 120 cw:k % 2 ? 200 : 236];
    k++;
  }
  y += REST_ROWS * ROW;
  self.status = [self field:@"Status" page:PG_SAMPLER x:8 y:y lw:120 cw:W - 140 editable:NO];
  y += ROW;
  self.settings = [self button:@"Settings" page:PG_SAMPLER x:132 y:y w:100];

  /* the SoundFont page, in Tab order after the tabs */
  y = 8 + ROW;
  self.sffile = [self field:@"SoundFont" page:PG_FONT x:8 y:y lw:120 cw:W - 140 editable:NO];
  y += ROW;
  self.sfbrowse = [self button:@"Browse" page:PG_FONT x:132 y:y w:100];
  self.file1 = [self popup:@"File" page:PG_FONT x:240 y:y lw:40 cw:470 items:nil];
  y += ROW;
  self.sfmode = [self popup:@"Mode" page:PG_FONT x:8 y:y lw:120 cw:200 items:@[@"All channels", @"One preset"]];
  y += ROW;
  NSUInteger mark = [self pageView:PG_FONT].subviews.count;
  self.sfpreset = [self popup:@"Preset" page:PG_FONT x:8 y:y lw:120 cw:400 items:nil];
  NSMutableArray *chs = [NSMutableArray arrayWithObject:@"All channels"];
  for (id = 1; id <= NCH; id++) [chs addObject:[NSString stringWithFormat:@"Channel %d", id]];
  self.sfchannel = [self popup:@"Channel" page:PG_FONT x:8 y:y + ROW lw:120 cw:200 items:chs];
  [self.modeOne addObjectsFromArray:[self subviewsSince:mark page:PG_FONT]];
  mark = [self pageView:PG_FONT].subviews.count;
  NSMutableArray *outs = [NSMutableArray array];
  for (int o = 1; o <= NCH; o++) [outs addObject:[NSString stringWithFormat:@"Output %d", o]];
  for (id = 0; id < NCH; id++) {
    int x = id < 8 ? 8 : 384, yy = y + (id % 8) * ROW;
    [self.chan addObject:[self popup:[NSString stringWithFormat:@"Channel %d", id + 1] page:PG_FONT x:x y:yy lw:66 cw:120 items:nil]];
    [self.outp addObject:[self popup:[NSString stringWithFormat:@"Channel %d output", id + 1] page:PG_FONT x:x + 194 y:yy lw:96 cw:76 items:outs]];
  }
  [self.modeAll addObjectsFromArray:[self subviewsSince:mark page:PG_FONT]];
  y += 8 * ROW;
  [self paramBox:SMP_P_SF_VOLUME page:PG_FONT x:8 y:y lw:120 cw:236];
  [self paramBox:SMP_P_SF_VOICES page:PG_FONT x:384 y:y lw:160 cw:200];
  y += ROW;
  [self paramBox:SMP_P_SF_TRANSPOSE page:PG_FONT x:8 y:y lw:120 cw:236];
  transpose_y = y;
  for (id = 0; id < NKNOB; id++) {               /* the knobs, in Tab order after SoundFont transpose */
    int x, yy;
    knob_place(id, y, &x, &yy);
    NSUInteger before = [self pageView:PG_FONT].subviews.count;
    SmpSlider *s = [self slider:[NSString stringWithFormat:@"Knob %d", id + 1] what:SL_KNOB0 - id page:PG_FONT x:x y:yy lw:120 cw:236];
    [self.knob addObject:s];
    [self.knobLab addObject:[self pageView:PG_FONT].subviews[before]];
  }
  {
    NSUInteger before = [self pageView:PG_FONT].subviews.count;
    [self paramBox:SMP_P_SF_EFFECTS page:PG_FONT x:384 y:y lw:160 cw:200];
    self.fxLab = [self pageView:PG_FONT].subviews[before];
  }
  y += ROW;
  {
    NSUInteger before = [self pageView:PG_FONT].subviews.count;
    self.sfstatus = [self field:@"Status" page:PG_FONT x:8 y:y lw:120 cw:W - 140 editable:NO];
    self.sfstatusLab = [self pageView:PG_FONT].subviews[before];
  }
  y += ROW;
  self.sfsettings = [self button:@"Settings" page:PG_FONT x:132 y:y w:100];

  /* Settings */
  y = 8;
  self.stretcher = [self popup:@"Stretcher" page:PG_SETTINGS x:8 y:y lw:140 cw:260 items:@[@"Signalsmith Stretch, fast", @"Rubber Band, slower"]];
  y += ROW;
  self.keeptone = [self popup:@"Keep the tone" page:PG_SETTINGS x:8 y:y lw:140 cw:260 items:@[@"Off", @"On"]];
  y += ROW;
  self.dir = [self field:@"Cache folder" page:PG_SETTINGS x:8 y:y lw:140 cw:470 editable:YES];
  self.dirbrowse = [self button:@"Browse folder" page:PG_SETTINGS x:630 y:y w:120];
  y += ROW;
  self.limit = [self slider:@"Cache size limit" what:SL_LIMIT page:PG_SETTINGS x:8 y:y lw:140 cw:160];
  y += ROW;
  {
    const int *r; int n = smp_raw_rates(&r);
    NSMutableArray *rates = [NSMutableArray array];
    for (int i = 0; i < n; i++) [rates addObject:[NSString stringWithFormat:@"%d Hz", r[i]]];
    self.rawrate = [self popup:@"Raw sample rate" page:PG_SETTINGS x:8 y:y lw:140 cw:260 items:rates];
  }
  y += ROW;
  self.rawch = [self popup:@"Raw channels" page:PG_SETTINGS x:8 y:y lw:140 cw:260 items:@[@"Mono", @"Stereo"]];
  y += ROW;
  self.rawfmt = [self popup:@"Raw sample format" page:PG_SETTINGS x:8 y:y lw:140 cw:260
                      items:@[@"16-bit integer", @"24-bit integer", @"32-bit integer", @"32-bit float", @"8-bit unsigned"]];
  y += ROW;
  self.rawend = [self popup:@"Raw byte order" page:PG_SETTINGS x:8 y:y lw:140 cw:260 items:@[@"Little endian", @"Big endian"]];
  y += ROW;
  self.ffmpeg = [self field:@"FFmpeg" page:PG_SETTINGS x:8 y:y lw:140 cw:470 editable:NO];
  self.ffbrowse = [self button:@"Browse for FFmpeg" page:PG_SETTINGS x:630 y:y w:120];
  y += ROW;
  self.used = [self field:@"Cache in use" page:PG_SETTINGS x:8 y:y lw:140 cw:470 editable:NO];
  y += ROW;
  self.clear = [self button:@"Clear cache" page:PG_SETTINGS x:152 y:y w:120];
  y += ROW;
  self.back = [self button:@"Back" page:PG_SETTINGS x:152 y:y w:120];

  for (id = 0; id < SMP_NALLPARAMS; id++) [self showParam:id force:YES];
  [self showPage:smp_plug_plays(P) ? PG_FONT : PG_SAMPLER to:nil];
}
/* the window comes to the front with the focus on a control (the last used, else the page's first), and VoiceOver is
   told: a host may open its plugin window without making it the key window */
- (void)takeFocus {
  NSWindow *w = self.view.window;
  if (closing || !w || self.view.hiddenOrHasHiddenAncestor) return;
  [w makeKeyAndOrderFront:nil];
  NSView *to = self.last && [self shows:self.last] && [self.last isDescendantOf:self.view] ? self.last : [self orderOf:page].firstObject;
  NSView *fr = owner_of(w.firstResponder);
  if (!fr || ![fr isDescendantOf:self.view]) [self focus:to]; else to = fr;
  NSAccessibilityPostNotification(to, NSAccessibilityFocusedUIElementChangedNotification);
}
- (void)attached {                                /* in the host's window: the timer, and the focus on the first control */
  [g_views addObject:self.view];
  if (self.timer) return;
  self.timer = [NSTimer timerWithTimeInterval:0.25 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
  [[NSRunLoop mainRunLoop] addTimer:self.timer forMode:NSDefaultRunLoopMode];
  /* Ctrl+Tab and Ctrl+Shift+Tab switch the player from any control here; the window would otherwise use them to move
     the focus itself before the control sees them. Only while the focus is in this window's controls. */
  __weak SmpGui *wk = self;
  self.keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *e) {
    SmpGui *g = wk;
    if (!g || g->closing || !g->P || e.keyCode != 48 || !(e.modifierFlags & NSEventModifierFlagControl)) return e;
    NSView *f = owner_of(e.window.firstResponder);
    if (!f || ![f isDescendantOf:g.view]) return e;
    if ([f isKindOfClass:[SmpField class]] && ((SmpField *)f).forSlider) [g editEnd:((SmpField *)f).forSlider apply:YES toSlider:NO];
    [g nextTab:(e.modifierFlags & NSEventModifierFlagShift) != 0];
    return nil;
  }];
  __weak SmpGui *w = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    SmpGui *g = w;
    if (g && !g->closing && g.view.window) [g takeFocus];
  });
}
- (void)close {
  closing = YES;
  [self.timer invalidate]; self.timer = nil;
  if (self.keyMonitor) { [NSEvent removeMonitor:self.keyMonitor]; self.keyMonitor = nil; }
  for (SmpSlider *s in self.sliders) s.gui = nil;
  [self.view removeFromSuperview];
  P = NULL;
}
@end

/* ---- the classes' methods that need the controller ---- */
@implementation SmpView (Drop)
- (BOOL)performDragOperation:(id<NSDraggingInfo>)s {
  NSArray *urls = [[s draggingPasteboard] readObjectsForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
  NSURL *u = urls.firstObject;
  SmpGui *g = self.gui;
  if (!u || !g) return NO;
  NSString *p = u.path;
  /* the load (maybe a decode) runs after the drag has returned, so the Finder isn't kept waiting */
  dispatch_async(dispatch_get_main_queue(), ^{ if (!g->closing && [g plug]) [g loadPath:p how:LOAD_DROPPED]; });
  return YES;
}
@end
@implementation SmpSlider (Access)
- (BOOL)accessibilityPerformIncrement { SmpGui *g = self.gui; if (!g || g->closing) return NO; [g nudge:self by:1]; return YES; }
- (BOOL)accessibilityPerformDecrement { SmpGui *g = self.gui; if (!g || g->closing) return NO; [g nudge:self by:-1]; return YES; }
@end
@implementation SmpPopUp (Focus)
- (BOOL)becomeFirstResponder {
  BOOL ok = [super becomeFirstResponder];
  if (ok) { self.gui.last = self; SmpGui *g = self.gui; if (g && !g->closing && (self == g.file0 || self == g.file1)) [g focusedFile:self]; }
  return ok;
}
- (void)menuDidClose:(NSMenu *)m {
  (void)m; self.menuOpen = NO;
  SmpGui *g = self.gui;
  if (g && !g->closing && (self == g.file0 || self == g.file1)) {
    dispatch_async(dispatch_get_main_queue(), ^{ if (!g->closing) [g fileList:self == g.file1 rescan:YES]; });
  }
}
@end

/* ================================================================ VoiceOver's focus in hosts that answer for it
   VoiceOver follows the keyboard by asking the application which element has the focus. Some hosts answer that
   themselves, with their own window's element, whatever window is in front: in Ableton Live the answer is always a
   control of Live's main window, so VoiceOver can't follow the focus into a plugin's window or into the file panel
   it opens, and reads nothing as one moves there. So, once, the host's own answer is wrapped: while the key window is
   one of Samplosis's windows (or a file panel Samplosis has open) the answer is the one AppKit itself gives (the
   element that has the keyboard focus in that window); in every other case it is the host's, unchanged. Only a host
   that answers for itself (its application class overrides the question) is touched. */
#import <objc/runtime.h>
/* g_views: Samplosis's views now in windows (weak); g_panels: file panels Samplosis has open */
static IMP g_host_focus, g_base_focus, g_host_attr, g_base_attr;

static BOOL ours_in_front(void) {
  NSWindow *kw = NSApp.keyWindow;
  if (!kw) return NO;
  if (g_panels > 0 && [kw isKindOfClass:[NSSavePanel class]]) return YES;
  for (NSView *v in g_views) if (v.window == kw) return YES;
  return NO;
}
static id smp_focused(id self, SEL cmd) {
  if (ours_in_front()) return ((id (*)(id, SEL))g_base_focus)(self, cmd);
  return ((id (*)(id, SEL))g_host_focus)(self, cmd);
}
static id smp_attr(id self, SEL cmd, NSString *a) {
  if ([a isEqualToString:NSAccessibilityFocusedUIElementAttribute] && ours_in_front())
    return ((id (*)(id, SEL, NSString *))g_base_attr)(self, cmd, a);
  return ((id (*)(id, SEL, NSString *))g_host_attr)(self, cmd, a);
}
static void focus_fix_install(void) {
  static BOOL done;
  if (done) return;
  done = YES;
  g_views = [NSHashTable weakObjectsHashTable];
  Class app = [NSApp class], base = [NSApplication class];
  if (!app || app == base) return;              /* a plain application: AppKit answers already */
  SEL f = @selector(accessibilityFocusedUIElement), a = @selector(accessibilityAttributeValue:);
  Method hf = class_getInstanceMethod(app, f), bf = class_getInstanceMethod(base, f);
  if (hf && bf && method_getImplementation(hf) != method_getImplementation(bf)) {
    g_base_focus = method_getImplementation(bf);
    g_host_focus = class_replaceMethod(app, f, (IMP)smp_focused, method_getTypeEncoding(hf));
    if (!g_host_focus) g_host_focus = method_getImplementation(hf);
  }
  Method ha = class_getInstanceMethod(app, a), ba = class_getInstanceMethod(base, a);
  if (ha && ba && method_getImplementation(ha) != method_getImplementation(ba)) {
    g_base_attr = method_getImplementation(ba);
    g_host_attr = class_replaceMethod(app, a, (IMP)smp_attr, method_getTypeEncoding(ha));
    if (!g_host_attr) g_host_attr = method_getImplementation(ha);
  }
}

/* ================================================================ the CLAP gui extension: embedded, Cocoa */
static SmpGui *ctl_of(Plug *P) { return P && P->gui ? (__bridge SmpGui *)P->gui->ctl : nil; }

void smp_gui_loaded(Plug *P) {
  SmpGui *g = ctl_of(P);
  if (g && !g->closing && !g->modal && g.view.window) [g refresh];
}
void smp_gui_close(Plug *P) {
  if (!P || !P->gui) return;
  struct Gui *gs = P->gui;
  SmpGui *g = (__bridge_transfer SmpGui *)gs->ctl;   /* the reference made in g_create ends with this scope */
  P->gui = NULL;
  free(gs);
  [g close];
}

static Plug *plug(const clap_plugin_t *p) { return (Plug *)p->plugin_data; }
static bool g_supported(const clap_plugin_t *p, const char *api, bool floating) { (void)p; return !strcmp(api, CLAP_WINDOW_API_COCOA) && !floating; }
static bool g_preferred(const clap_plugin_t *p, const char **api, bool *floating) { (void)p; *api = CLAP_WINDOW_API_COCOA; *floating = false; return true; }
static bool g_create(const clap_plugin_t *p, const char *api, bool floating) {
  Plug *P = plug(p);
  if (!g_supported(p, api, floating)) return false;
  focus_fix_install();
  if (P->gui) return true;
  struct Gui *gs = calloc(1, sizeof *gs);
  if (!gs) return false;
  @autoreleasepool {
    SmpGui *g = [[SmpGui alloc] init];
    g->P = P;
    g->fails_seen = P->load_failed;              /* failures from before the window opened aren't told again */
    g->notes_seen = P->load_notes;
    gs->ctl = (__bridge_retained void *)g;
    P->gui = gs;
    [g build];
  }
  return true;
}
static void g_destroy(const clap_plugin_t *p) { smp_gui_close(plug(p)); }
static bool g_scale(const clap_plugin_t *p, double s) { (void)p; (void)s; return false; }
static bool g_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { (void)p; *w = W_WIDTH; *h = W_HEIGHT; return true; }
static bool g_can_resize(const clap_plugin_t *p) { (void)p; return false; }
static bool g_hints(const clap_plugin_t *p, clap_gui_resize_hints_t *h) { (void)p; (void)h; return false; }
static bool g_adjust(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { return g_size(p, w, h); }
static bool g_set_size(const clap_plugin_t *p, uint32_t w, uint32_t h) { (void)p; (void)w; (void)h; return true; }
static bool g_parent(const clap_plugin_t *p, const clap_window_t *win) {
  SmpGui *g = ctl_of(plug(p));
  NSView *parent = (__bridge NSView *)win->cocoa;
  if (!g || !parent) return false;
  [parent addSubview:g.view];
  [g attached];
  return true;
}
static bool g_transient(const clap_plugin_t *p, const clap_window_t *w) { (void)p; (void)w; return false; }
static void g_title(const clap_plugin_t *p, const char *t) { (void)p; (void)t; }
static bool g_show(const clap_plugin_t *p) {
  SmpGui *g = ctl_of(plug(p));
  if (!g) return true;
  g.view.hidden = NO;
  __weak SmpGui *w = g;                        /* once the host has its window on screen */
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    SmpGui *gg = w; if (gg && !gg->closing) [gg takeFocus];
  });
  return true;
}
static bool g_hide(const clap_plugin_t *p) { SmpGui *g = ctl_of(plug(p)); if (g) g.view.hidden = YES; return true; }

const clap_plugin_gui_t smp_gui_ext = {g_supported, g_preferred, g_create, g_destroy, g_scale, g_size, g_can_resize,
                                       g_hints, g_adjust, g_set_size, g_parent, g_transient, g_title, g_show, g_hide};
