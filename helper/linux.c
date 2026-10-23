// paseo-screen-helper for Linux: captures the screen and injects input for the paseo-screen plugin.
// It speaks the same protocol as helper/main.swift. One source builds two helpers: the default build
// talks to an X11 session directly; with -DPASEO_WAYLAND it goes through the desktop portals
// (RemoteDesktop and ScreenCast over PipeWire), which is the only way in on a Wayland session.
//
// Protocol: one JSON object per line on stdin, one JSON reply per line on stdout.
//   {"id": 1, "cmd": "status"}
//   {"id": 2, "cmd": "frame", "display": 1, "maxWidth": 1600, "quality": 0.6, "since": "abc"}
//   {"id": 3, "cmd": "input", "display": 1, "events": [{"type": "click", "x": 0.5, "y": 0.5}]}
//   {"id": 4, "cmd": "clipboard"}
// Coordinates in input events are normalized to 0...1 within the chosen display. Key codes are macOS
// virtual key codes (physical key positions) and are pressed as the key in the same position here.
// The "cmd" modifier is the Super key.
//
// Build (X11):     cc -O2 -std=gnu11 -o paseo-screen-helper linux.c -lX11 -lXext -lXtst -lXrandr -lXfixes -ljpeg -lpthread -lm
// Build (Wayland): cc -O2 -std=gnu11 -DPASEO_WAYLAND -o paseo-screen-helper linux.c
//                    $(pkg-config --cflags --libs gio-unix-2.0 gstreamer-app-1.0 gstreamer-video-1.0) -ljpeg -lpthread -lm

#define _GNU_SOURCE
#ifndef PASEO_WAYLAND
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/dpms.h>
#include <X11/keysym.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PASEO_WAYLAND
#include <sys/ipc.h>
#include <sys/shm.h>
#endif
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef PASEO_WAYLAND
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
// Key symbols are plain X11 keysym values; the portal takes the same numbers.
typedef unsigned long KeySym;
#define XK_BackSpace 0xff08
#define XK_Tab 0xff09
#define XK_Return 0xff0d
#endif
// jpeglib.h uses FILE from stdio.h without including it.
#include <jpeglib.h>

// MARK: - Output buffer

typedef struct {
  char *data;
  size_t len, cap;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
  if (b->len + extra + 1 <= b->cap) return;
  size_t cap = b->cap ? b->cap * 2 : 256;
  while (cap < b->len + extra + 1) cap *= 2;
  b->data = realloc(b->data, cap);
  if (!b->data) abort();
  b->cap = cap;
}

static void buf_put(Buf *b, const char *s, size_t n) {
  buf_reserve(b, n);
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = 0;
}

static void buf_puts(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

static void buf_printf(Buf *b, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int n = vsnprintf(NULL, 0, format, args);
  va_end(args);
  if (n < 0) return;
  buf_reserve(b, (size_t)n);
  va_start(args, format);
  vsnprintf(b->data + b->len, (size_t)n + 1, format, args);
  va_end(args);
  b->len += (size_t)n;
}

static void buf_json_string(Buf *b, const char *s, size_t n) {
  buf_put(b, "\"", 1);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"': buf_put(b, "\\\"", 2); break;
      case '\\': buf_put(b, "\\\\", 2); break;
      case '\n': buf_put(b, "\\n", 2); break;
      case '\r': buf_put(b, "\\r", 2); break;
      case '\t': buf_put(b, "\\t", 2); break;
      default:
        if (c < 0x20) buf_printf(b, "\\u%04x", c);
        else buf_put(b, (const char *)&c, 1);
    }
  }
  buf_put(b, "\"", 1);
}

static void buf_free(Buf *b) {
  free(b->data);
  *b = (Buf){0};
}

static void append_utf8(Buf *b, uint32_t cp) {
  char out[4];
  size_t n;
  if (cp < 0x80) {
    out[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    n = 4;
  }
  buf_put(b, out, n);
}

/// Decodes one UTF-8 code point at `*s`, advancing it. Invalid bytes decode as U+FFFD.
static uint32_t next_codepoint(const char **s, const char *end) {
  const unsigned char *p = (const unsigned char *)*s;
  uint32_t cp = 0xFFFD;
  size_t n = 1;
  if (p[0] < 0x80) {
    cp = p[0];
  } else if ((p[0] & 0xE0) == 0xC0 && end - *s >= 2 && (p[1] & 0xC0) == 0x80) {
    cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    n = 2;
  } else if ((p[0] & 0xF0) == 0xE0 && end - *s >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
    cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    n = 3;
  } else if ((p[0] & 0xF8) == 0xF0 && end - *s >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
             (p[3] & 0xC0) == 0x80) {
    cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) |
         (p[3] & 0x3F);
    n = 4;
  }
  *s += n;
  return cp;
}

// MARK: - JSON requests

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JsonType;

typedef struct Json Json;
struct Json {
  JsonType type;
  double num;  // J_NUM, and J_BOOL as 0 or 1
  char *str;   // J_STR, NUL-terminated UTF-8
  size_t len;  // J_STR byte length
  size_t count;
  char **keys;   // J_OBJ
  Json **items;  // J_ARR and J_OBJ values
};

typedef struct {
  const char *p;
} Parser;

static void json_free(Json *j) {
  if (!j) return;
  for (size_t i = 0; i < j->count; i++) {
    if (j->keys) free(j->keys[i]);
    json_free(j->items[i]);
  }
  free(j->keys);
  free(j->items);
  free(j->str);
  free(j);
}

static void skip_space(Parser *ps) {
  while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ps->p++;
}

static int hex4(const char *p) {
  int value = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    value <<= 4;
    if (c >= '0' && c <= '9') value |= c - '0';
    else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
    else return -1;
  }
  return value;
}

static char *parse_string(Parser *ps, size_t *length) {
  if (*ps->p != '"') return NULL;
  ps->p++;
  Buf b = {0};
  buf_reserve(&b, 0);
  for (;;) {
    char c = *ps->p++;
    if (c == 0) {
      buf_free(&b);
      return NULL;
    }
    if (c == '"') break;
    if (c != '\\') {
      buf_put(&b, &c, 1);
      continue;
    }
    char e = *ps->p++;
    switch (e) {
      case '"': case '\\': case '/': buf_put(&b, &e, 1); break;
      case 'b': buf_put(&b, "\b", 1); break;
      case 'f': buf_put(&b, "\f", 1); break;
      case 'n': buf_put(&b, "\n", 1); break;
      case 'r': buf_put(&b, "\r", 1); break;
      case 't': buf_put(&b, "\t", 1); break;
      case 'u': {
        int unit = hex4(ps->p);
        if (unit < 0) {
          buf_free(&b);
          return NULL;
        }
        ps->p += 4;
        uint32_t cp = (uint32_t)unit;
        if (unit >= 0xD800 && unit <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
          int low = hex4(ps->p + 2);
          if (low >= 0xDC00 && low <= 0xDFFF) {
            cp = 0x10000 + (((uint32_t)unit - 0xD800) << 10) + ((uint32_t)low - 0xDC00);
            ps->p += 6;
          }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        append_utf8(&b, cp);
        break;
      }
      default:
        buf_free(&b);
        return NULL;
    }
  }
  *length = b.len;
  return b.data;
}

static Json *parse_value(Parser *ps, int depth);

static Json *json_new(JsonType type) {
  Json *j = calloc(1, sizeof *j);
  if (!j) abort();
  j->type = type;
  return j;
}

static void json_push(Json *j, char *key, Json *item) {
  j->items = realloc(j->items, (j->count + 1) * sizeof *j->items);
  if (j->type == J_OBJ) j->keys = realloc(j->keys, (j->count + 1) * sizeof *j->keys);
  if (!j->items || (j->type == J_OBJ && !j->keys)) abort();
  if (j->type == J_OBJ) j->keys[j->count] = key;
  j->items[j->count++] = item;
}

static Json *parse_container(Parser *ps, int depth, bool object) {
  Json *j = json_new(object ? J_OBJ : J_ARR);
  char close = object ? '}' : ']';
  ps->p++;
  skip_space(ps);
  if (*ps->p == close) {
    ps->p++;
    return j;
  }
  for (;;) {
    char *key = NULL;
    skip_space(ps);
    if (object) {
      size_t length;
      key = parse_string(ps, &length);
      skip_space(ps);
      if (!key || *ps->p != ':') {
        free(key);
        json_free(j);
        return NULL;
      }
      ps->p++;
    }
    Json *item = parse_value(ps, depth + 1);
    if (!item) {
      free(key);
      json_free(j);
      return NULL;
    }
    json_push(j, key, item);
    skip_space(ps);
    if (*ps->p == ',') {
      ps->p++;
      continue;
    }
    if (*ps->p == close) {
      ps->p++;
      return j;
    }
    json_free(j);
    return NULL;
  }
}

static Json *parse_value(Parser *ps, int depth) {
  if (depth > 32) return NULL;
  skip_space(ps);
  char c = *ps->p;
  if (c == '{' || c == '[') return parse_container(ps, depth, c == '{');
  if (c == '"') {
    Json *j = json_new(J_STR);
    j->str = parse_string(ps, &j->len);
    if (!j->str) {
      json_free(j);
      return NULL;
    }
    return j;
  }
  if (strncmp(ps->p, "true", 4) == 0 || strncmp(ps->p, "false", 5) == 0) {
    Json *j = json_new(J_BOOL);
    j->num = c == 't';
    ps->p += c == 't' ? 4 : 5;
    return j;
  }
  if (strncmp(ps->p, "null", 4) == 0) {
    ps->p += 4;
    return json_new(J_NULL);
  }
  char *end;
  double value = strtod(ps->p, &end);
  if (end == ps->p) return NULL;
  ps->p = end;
  Json *j = json_new(J_NUM);
  j->num = value;
  return j;
}

static Json *json_get(const Json *object, const char *key) {
  if (!object || object->type != J_OBJ) return NULL;
  for (size_t i = 0; i < object->count; i++)
    if (strcmp(object->keys[i], key) == 0) return object->items[i];
  return NULL;
}

static double json_num(const Json *j, double fallback) { return j && j->type == J_NUM ? j->num : fallback; }
static bool json_bool(const Json *j, bool fallback) { return j && j->type == J_BOOL ? j->num != 0 : fallback; }
static const char *json_str(const Json *j) { return j && j->type == J_STR ? j->str : NULL; }

// MARK: - Errors

// Per thread: the Wayland helper answers frame requests on a thread of their own.
static __thread char error_message[1024];

static bool fail(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(error_message, sizeof error_message, format, args);
  va_end(args);
  return false;
}

/// Set when the helper cannot reach the desktop at all; every command then fails with it.
static const char *open_error;

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#ifndef PASEO_WAYLAND
// MARK: - X connection

static Display *dpy;
static Window root;
static int screen_number;
static bool has_xtest, has_shm, has_dpms, has_monitors, has_fixes;

/// X errors would otherwise end the process. They are recorded for the connection that caused them.
static Display *clip_dpy;
static volatile bool main_x_error;

static int on_x_error(Display *display, XErrorEvent *event) {
  (void)event;
  if (display == dpy) main_x_error = true;
  return 0;
}
#endif

// MARK: - Displays

typedef struct {
  unsigned long id;
  int x, y, width, height;   // desktop coordinates; input positions map onto this rectangle
  int pixel_width, pixel_height;  // frame size in pixels when it differs (0: same as width and height)
  bool main, builtin;
  char name[96];
} Monitor;

#define MAX_MONITORS 16

#ifndef PASEO_WAYLAND
/// Monitor name from the EDID descriptor (0xFC), as the desktop's display settings show it.
static bool edid_name(RROutput output, char *name, size_t size) {
  Atom edid = XInternAtom(dpy, "EDID", True);
  if (edid == None) return false;
  Atom type;
  int format;
  unsigned long count, after;
  unsigned char *data = NULL;
  if (XRRGetOutputProperty(dpy, output, edid, 0, 64, False, False, AnyPropertyType, &type, &format, &count, &after,
                           &data) != Success || !data)
    return false;
  bool found = false;
  if (format == 8 && count >= 128) {
    for (int offset = 54; offset <= 108 && !found; offset += 18) {
      const unsigned char *d = data + offset;
      if (d[0] || d[1] || d[2] || d[3] != 0xFC) continue;
      size_t n = 0;
      for (int i = 5; i < 18 && d[i] != 0x0A && n + 1 < size; i++) name[n++] = (char)d[i];
      while (n > 0 && name[n - 1] == ' ') n--;
      name[n] = 0;
      found = n > 0;
    }
  }
  XFree(data);
  return found;
}

/// Laptop panels report the RandR connector type "Panel", whatever the driver names the connector
/// (NVIDIA calls some of them DP-n). Older drivers lack the property, so the eDP/LVDS/DSI names count too.
static bool is_builtin(RROutput output, const char *connector) {
  if (!strncmp(connector, "eDP", 3) || !strncmp(connector, "LVDS", 4) || !strncmp(connector, "DSI", 3)) return true;
  Atom property = XInternAtom(dpy, "ConnectorType", True);
  if (property == None) return false;
  Atom type;
  int format;
  unsigned long count, after;
  unsigned char *data = NULL;
  if (XRRGetOutputProperty(dpy, output, property, 0, 1, False, False, XA_ATOM, &type, &format, &count, &after, &data) != Success ||
      !data)
    return false;
  bool panel = false;
  if (type == XA_ATOM && format == 32 && count == 1) {
    char *name = XGetAtomName(dpy, (Atom)((unsigned long *)data)[0]);
    panel = name && !strcmp(name, "Panel");
    if (name) XFree(name);
  }
  XFree(data);
  return panel;
}

static int list_monitors(Monitor *out) {
  int count = 0;
  if (has_monitors) {
    int n = 0;
    XRRMonitorInfo *monitors = XRRGetMonitors(dpy, root, True, &n);
    XRRScreenResources *resources = XRRGetScreenResourcesCurrent(dpy, root);
    for (int i = 0; i < n && count < MAX_MONITORS; i++) {
      XRRMonitorInfo *m = &monitors[i];
      if (m->width <= 0 || m->height <= 0) continue;
      // A mirrored display shows the same picture as its source, so only the source is listed.
      bool duplicate = false;
      for (int k = 0; k < count; k++)
        if (out[k].x == m->x && out[k].y == m->y && out[k].width == m->width && out[k].height == m->height) duplicate = true;
      if (duplicate) continue;
      Monitor *d = &out[count++];
      *d = (Monitor){.x = m->x, .y = m->y, .width = m->width, .height = m->height, .main = m->primary};
      RROutput output = m->noutput > 0 ? m->outputs[0] : None;
      d->id = output != None ? output : m->name;
      char connector[64] = "";
      if (output != None && resources) {
        XRROutputInfo *info = XRRGetOutputInfo(dpy, resources, output);
        if (info) {
          snprintf(connector, sizeof connector, "%s", info->name);
          XRRFreeOutputInfo(info);
        }
        d->builtin = is_builtin(output, connector);
      }
      // Laptop panels rarely carry a monitor name in their EDID, so they are named like the macOS fallback.
      if (output == None || !edid_name(output, d->name, sizeof d->name)) {
        if (d->builtin) snprintf(d->name, sizeof d->name, "Built-in Display");
        else if (connector[0]) snprintf(d->name, sizeof d->name, "%s", connector);
        else snprintf(d->name, sizeof d->name, "Display %lu", d->id);
      }
    }
    if (resources) XRRFreeScreenResources(resources);
    if (monitors) XRRFreeMonitors(monitors);
  }
  if (count == 0) {
    out[0] = (Monitor){.id = 1, .width = DisplayWidth(dpy, screen_number), .height = DisplayHeight(dpy, screen_number), .main = true};
    snprintf(out[0].name, sizeof out[0].name, "Screen");
    count = 1;
  }
  bool any_main = false;
  for (int i = 0; i < count; i++) any_main |= out[i].main;
  if (!any_main) out[0].main = true;
  return count;
}
#endif

static void monitor_json(Buf *b, const Monitor *d) {
  buf_printf(b, "{\"id\":%lu,\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"pixelWidth\":%d,\"pixelHeight\":%d,\"main\":%s,\"builtin\":%s,\"name\":",
             d->id, d->x, d->y, d->width, d->height, d->pixel_width ? d->pixel_width : d->width,
             d->pixel_height ? d->pixel_height : d->height, d->main ? "true" : "false", d->builtin ? "true" : "false");
  buf_json_string(b, d->name, strlen(d->name));
  buf_puts(b, "}");
}

static const Monitor *resolve_monitor(const Monitor *monitors, int count, const Json *requested) {
  double number = json_num(requested, 0);
  if (number != 0)
    for (int i = 0; i < count; i++)
      if ((double)monitors[i].id == number) return &monitors[i];
  for (int i = 0; i < count; i++)
    if (monitors[i].main) return &monitors[i];
  return &monitors[0];
}

static const Monitor *monitor_containing(const Monitor *monitors, int count, int x, int y) {
  for (int i = 0; i < count; i++) {
    const Monitor *d = &monitors[i];
    if (x >= d->x && x < d->x + d->width && y >= d->y && y < d->y + d->height) return d;
  }
  return NULL;
}

// MARK: - Sleep and lock

#ifndef PASEO_WAYLAND
/// The displays are powered down by DPMS (the desktop blanked them after being idle).
static bool displays_asleep(void) {
  if (!has_dpms) return false;
  CARD16 level;
  BOOL enabled;
  if (!DPMSInfo(dpy, &level, &enabled)) return false;
  return enabled && level != DPMSModeOn;
}

/// Wakes sleeping displays the way user input would, and waits briefly for them to come back.
/// Called only for an explicit request (opening the viewer, the Wake button, or input), never from
/// ordinary frame polling, so an open viewer does not keep the host from sleeping or locking.
static void wake_displays(void) {
  // A small pointer nudge counts as user activity for the desktop's idle tracking.
  if (has_xtest) {
    XTestFakeRelativeMotionEvent(dpy, 1, 0, CurrentTime);
    XTestFakeRelativeMotionEvent(dpy, -1, 0, CurrentTime);
  }
  XForceScreenSaver(dpy, ScreenSaverReset);
  if (has_dpms) DPMSForceLevel(dpy, DPMSModeOn);
  XSync(dpy, False);
  for (int i = 0; i < 30 && displays_asleep(); i++) usleep(100000);
}
#endif

/// Asks the desktop's screen saver over D-Bus: 1 active, 0 inactive, -1 no answer.
static int screensaver_active(const char *service, const char *path) {
  char command[512];
  snprintf(command, sizeof command,
           "gdbus call --session --timeout 1 --dest %s --object-path %s --method %s.GetActive </dev/null 2>/dev/null",
           service, path, service);
  FILE *pipe = popen(command, "r");
  if (!pipe) return -1;
  char output[128] = "";
  size_t n = fread(output, 1, sizeof output - 1, pipe);
  int status = pclose(pipe);
  if (status != 0 || n == 0) return -1;
  return strstr(output, "true") != NULL;
}

#ifndef PASEO_WAYLAND
/// True when DISPLAY is served by a virtual X server (Xvfb, Xvnc, Xephyr). Such a display has no lock
/// screen of its own, and the screensaver on the session bus belongs to another session: on a home
/// server with a virtual desktop, that is the user's GNOME login, whose lock would otherwise show here.
static bool virtual_display(void) {
  static int cached = -1;
  if (cached >= 0) return cached;
  cached = 0;
  const char *display = getenv("DISPLAY");
  const char *colon = display ? strchr(display, ':') : NULL;
  if (!colon) return false;
  char wanted[32];
  snprintf(wanted, sizeof wanted, ":%.*s", (int)strcspn(colon + 1, "."), colon + 1);
  DIR *proc = opendir("/proc");
  struct dirent *entry;
  while (proc && (entry = readdir(proc))) {
    if (!isdigit((unsigned char)entry->d_name[0])) continue;
    char path[300], args[4096];
    snprintf(path, sizeof path, "/proc/%s/cmdline", entry->d_name);
    FILE *file = fopen(path, "r");
    if (!file) continue;
    size_t n = fread(args, 1, sizeof args - 1, file);
    fclose(file);
    args[n] = 0;
    const char *name = strrchr(args, '/') ? strrchr(args, '/') + 1 : args;
    if (strcmp(name, "Xvfb") && strcmp(name, "Xvnc") && strcmp(name, "Xtigervnc") && strcmp(name, "Xephyr")) continue;
    for (size_t i = strlen(args) + 1; i < n; i += strlen(args + i) + 1)
      if (!strcmp(args + i, wanted)) cached = 1;
    if (cached) break;
  }
  if (proc) closedir(proc);
  return cached;
}
#endif

/// True while the session's lock screen is showing. Checked at most every two seconds.
static bool screen_locked(void) {
#ifndef PASEO_WAYLAND
  if (virtual_display()) return false;
#endif
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static double checked_at = -100;
  static bool locked = false;
  pthread_mutex_lock(&lock);
  double now = now_seconds();
  if (now - checked_at >= 2) {
    checked_at = now;
    int active = screensaver_active("org.gnome.ScreenSaver", "/org/gnome/ScreenSaver");
    if (active < 0) active = screensaver_active("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver");
    locked = active == 1;
  }
  bool result = locked;
  pthread_mutex_unlock(&lock);
  return result;
}

// MARK: - Capture

#ifndef PASEO_WAYLAND
static XImage *shm_image;
static XShmSegmentInfo shm_info;

static void free_shm_image(void) {
  if (!shm_image) return;
  XShmDetach(dpy, &shm_info);
  XSync(dpy, False);
  XDestroyImage(shm_image);
  shmdt(shm_info.shmaddr);
  shm_image = NULL;
}

/// Reads a rectangle of the root window. Returns the shared-memory image when possible.
static XImage *grab(int x, int y, int width, int height) {
  if (has_shm) {
    if (shm_image && (shm_image->width != width || shm_image->height != height)) free_shm_image();
    if (!shm_image) {
      Visual *visual = DefaultVisual(dpy, screen_number);
      int depth = DefaultDepth(dpy, screen_number);
      shm_image = XShmCreateImage(dpy, visual, (unsigned)depth, ZPixmap, NULL, &shm_info, (unsigned)width, (unsigned)height);
      if (shm_image) {
        shm_info.shmid = shmget(IPC_PRIVATE, (size_t)shm_image->bytes_per_line * (size_t)height, IPC_CREAT | 0600);
        if (shm_info.shmid < 0) {
          XDestroyImage(shm_image);
          shm_image = NULL;
          has_shm = false;
        } else {
          shm_info.shmaddr = shm_image->data = shmat(shm_info.shmid, NULL, 0);
          shm_info.readOnly = False;
          main_x_error = false;
          XShmAttach(dpy, &shm_info);
          XSync(dpy, False);
          shmctl(shm_info.shmid, IPC_RMID, NULL);
          if (main_x_error) {
            // A remote X server cannot share memory with us; fall back to plain reads.
            XDestroyImage(shm_image);
            shmdt(shm_info.shmaddr);
            shm_image = NULL;
            has_shm = false;
          }
        }
      }
    }
    if (shm_image) {
      main_x_error = false;
      if (XShmGetImage(dpy, root, shm_image, x, y, AllPlanes) && !main_x_error) return shm_image;
    }
  }
  main_x_error = false;
  XImage *image = XGetImage(dpy, root, x, y, (unsigned)width, (unsigned)height, AllPlanes, ZPixmap);
  XSync(dpy, False);
  return main_x_error ? NULL : image;
}
#endif

#ifndef PASEO_WAYLAND
static int mask_shift(unsigned long mask) {
  int shift = 0;
  while (mask && !(mask & 1)) {
    mask >>= 1;
    shift++;
  }
  return shift;
}
#endif

#ifndef PASEO_WAYLAND
/// Draws the pointer into the captured image, as the macOS capture does with showsCursor.
static void draw_cursor(XImage *image, const Monitor *d) {
  if (!has_fixes) return;
  XFixesCursorImage *cursor = XFixesGetCursorImage(dpy);
  if (!cursor) return;
  int left = cursor->x - cursor->xhot - d->x;
  int top = cursor->y - cursor->yhot - d->y;
  int rs = mask_shift(image->red_mask), gs = mask_shift(image->green_mask), bs = mask_shift(image->blue_mask);
  for (int cy = 0; cy < cursor->height; cy++) {
    int y = top + cy;
    if (y < 0 || y >= image->height) continue;
    uint32_t *row = (uint32_t *)(image->data + (size_t)y * (size_t)image->bytes_per_line);
    for (int cx = 0; cx < cursor->width; cx++) {
      int x = left + cx;
      if (x < 0 || x >= image->width) continue;
      unsigned long p = cursor->pixels[cy * cursor->width + cx];
      uint32_t a = (p >> 24) & 0xFF;
      if (a == 0) continue;
      uint32_t dst = row[x];
      // Cursor pixels are premultiplied ARGB.
      uint32_t r = ((p >> 16) & 0xFF) + (((dst >> rs) & 0xFF) * (255 - a)) / 255;
      uint32_t g = ((p >> 8) & 0xFF) + (((dst >> gs) & 0xFF) * (255 - a)) / 255;
      uint32_t b = (p & 0xFF) + (((dst >> bs) & 0xFF) * (255 - a)) / 255;
      row[x] = (r > 255 ? 255 : r) << rs | (g > 255 ? 255 : g) << gs | (b > 255 ? 255 : b) << bs;
    }
  }
  XFree(cursor);
}
#endif

/// Scales 32-bit pixels (channels at bit shifts rs, gs, bs) down to width x height RGB by averaging
/// each source block.
static uint8_t *scale_to_rgb(const uint8_t *data, size_t stride, int src_width, int src_height, int rs, int gs, int bs,
                             int width, int height) {
  uint8_t *rgb = malloc((size_t)width * (size_t)height * 3);
  int *columns = malloc(((size_t)width + 1) * sizeof *columns);
  if (!rgb || !columns) abort();
  for (int i = 0; i <= width; i++) columns[i] = (int)((long long)i * src_width / width);
  for (int oy = 0; oy < height; oy++) {
    int y0 = (int)((long long)oy * src_height / height);
    int y1 = (int)((long long)(oy + 1) * src_height / height);
    if (y1 <= y0) y1 = y0 + 1;
    uint8_t *out = rgb + (size_t)oy * (size_t)width * 3;
    for (int ox = 0; ox < width; ox++) {
      int x0 = columns[ox], x1 = columns[ox + 1];
      if (x1 <= x0) x1 = x0 + 1;
      uint32_t r = 0, g = 0, b = 0, n = 0;
      for (int y = y0; y < y1; y++) {
        const uint32_t *row = (const uint32_t *)(data + (size_t)y * stride);
        for (int x = x0; x < x1; x++) {
          uint32_t p = row[x];
          r += (p >> rs) & 0xFF;
          g += (p >> gs) & 0xFF;
          b += (p >> bs) & 0xFF;
        }
        n += (uint32_t)(x1 - x0);
      }
      out[ox * 3] = (uint8_t)(r / n);
      out[ox * 3 + 1] = (uint8_t)(g / n);
      out[ox * 3 + 2] = (uint8_t)(b / n);
    }
  }
  free(columns);
  return rgb;
}

struct jpeg_failure {
  struct jpeg_error_mgr manager;
  jmp_buf jump;
};

static void on_jpeg_error(j_common_ptr info) { longjmp(((struct jpeg_failure *)info->err)->jump, 1); }

static bool encode_jpeg(const uint8_t *rgb, int width, int height, int quality, unsigned char **out, unsigned long *size) {
  struct jpeg_compress_struct info;
  struct jpeg_failure failure;
  info.err = jpeg_std_error(&failure.manager);
  failure.manager.error_exit = on_jpeg_error;
  *out = NULL;
  *size = 0;
  if (setjmp(failure.jump)) {
    jpeg_destroy_compress(&info);
    free(*out);
    *out = NULL;
    return false;
  }
  jpeg_create_compress(&info);
  jpeg_mem_dest(&info, out, size);
  info.image_width = (JDIMENSION)width;
  info.image_height = (JDIMENSION)height;
  info.input_components = 3;
  info.in_color_space = JCS_RGB;
  jpeg_set_defaults(&info);
  jpeg_set_quality(&info, quality, TRUE);
  info.dct_method = JDCT_IFAST;
  jpeg_start_compress(&info, TRUE);
  while (info.next_scanline < info.image_height) {
    JSAMPROW row = (JSAMPROW)(rgb + (size_t)info.next_scanline * (size_t)width * 3);
    jpeg_write_scanlines(&info, &row, 1);
  }
  jpeg_finish_compress(&info);
  jpeg_destroy_compress(&info);
  return true;
}

static void base64(Buf *b, const unsigned char *data, size_t n) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  buf_reserve(b, (n + 2) / 3 * 4);
  char *out = b->data + b->len;
  size_t i = 0, o = 0;
  for (; i + 2 < n; i += 3) {
    uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8 | data[i + 2];
    out[o++] = table[v >> 18];
    out[o++] = table[(v >> 12) & 63];
    out[o++] = table[(v >> 6) & 63];
    out[o++] = table[v & 63];
  }
  if (i < n) {
    uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < n ? (uint32_t)data[i + 1] << 8 : 0);
    out[o++] = table[v >> 18];
    out[o++] = table[(v >> 12) & 63];
    out[o++] = i + 1 < n ? table[(v >> 6) & 63] : '=';
    out[o++] = '=';
  }
  b->len += o;
  b->data[b->len] = 0;
}

static uint64_t fnv1a(const unsigned char *data, size_t n) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < n; i++) {
    hash ^= data[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

/// Scales, encodes and writes one frame reply for display `d` from 32-bit pixels.
static bool frame_reply(Buf *out, const Monitor *d, const uint8_t *data, size_t stride, int src_width, int src_height, int rs,
                        int gs, int bs, const Json *request) {
  double max_width = json_num(json_get(request, "maxWidth"), 1600);
  if (max_width < 320) max_width = 320;
  double quality = json_num(json_get(request, "quality"), 0.6);
  quality = quality < 0.1 ? 0.1 : quality > 1 ? 1 : quality;
  double scale = max_width / src_width < 1 ? max_width / src_width : 1;
  int width = (int)(src_width * scale), height = (int)(src_height * scale);
  if (width < 1) width = 1;
  if (height < 1) height = 1;
  uint8_t *rgb = scale_to_rgb(data, stride, src_width, src_height, rs, gs, bs, width, height);
  unsigned char *jpeg = NULL;
  unsigned long size = 0;
  bool encoded = encode_jpeg(rgb, width, height, (int)lround(quality * 100), &jpeg, &size);
  free(rgb);
  if (!encoded) return fail("JPEG encoding failed");
  char hash[24];
  snprintf(hash, sizeof hash, "%llx", (unsigned long long)fnv1a(jpeg, size));

  buf_puts(out, "\"display\":");
  monitor_json(out, d);
  buf_printf(out, ",\"imageWidth\":%d,\"imageHeight\":%d,\"hash\":\"%s\",\"bytes\":%lu,\"locked\":%s", width, height, hash, size,
             screen_locked() ? "true" : "false");
  const char *since = json_str(json_get(request, "since"));
  if (since && strcmp(since, hash) == 0) {
    buf_puts(out, ",\"unchanged\":true");
  } else {
    buf_puts(out, ",\"jpeg\":\"");
    base64(out, jpeg, size);
    buf_puts(out, "\"");
  }
  free(jpeg);
  return true;
}

#ifndef PASEO_WAYLAND
static bool capture_frame(const Json *request, Buf *out) {
  Monitor monitors[MAX_MONITORS];
  if (displays_asleep()) {
    // Polling never wakes the host; the viewer shows "asleep" and offers a Wake button.
    if (!json_bool(json_get(request, "wake"), false)) {
      buf_printf(out, "\"asleep\":true,\"locked\":%s", screen_locked() ? "true" : "false");
      return true;
    }
    wake_displays();
  }
  int count = list_monitors(monitors);
  const Monitor *d = resolve_monitor(monitors, count, json_get(request, "display"));
  XImage *image = grab(d->x, d->y, d->width, d->height);
  if (!image) return fail("could not read the screen of display %lu", d->id);
  if (image->bits_per_pixel != 32 || image->byte_order != LSBFirst) {
    int bits = image->bits_per_pixel;
    if (image != shm_image) XDestroyImage(image);
    return fail("unsupported screen pixel format (%d bits per pixel)", bits);
  }
  draw_cursor(image, d);
  bool ok = frame_reply(out, d, (const uint8_t *)image->data, (size_t)image->bytes_per_line, image->width, image->height,
                        mask_shift(image->red_mask), mask_shift(image->green_mask), mask_shift(image->blue_mask), request);
  if (image != shm_image) XDestroyImage(image);
  return ok;
}

// MARK: - Clipboard


// The CLIPBOARD selection is served from its own connection and thread, because another app may ask
// for text this helper put there at any time, not only while a request is being handled.
static Window clip_window;
static Atom atom_clipboard, atom_utf8, atom_targets, atom_text, atom_incr, atom_property;
static int fixes_event_base;
static pthread_mutex_t clip_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clip_changed = PTHREAD_COND_INITIALIZER;
static int clip_pipe[2] = {-1, -1};
static bool clip_started;
static char *clip_text;  // text this helper owns on the clipboard
static size_t clip_text_length;
static bool clip_owner;
static unsigned long clip_change_count;
static bool clip_set_pending;
static bool clip_set_ok;
static bool clip_read_pending, clip_read_incr, clip_read_ok;
static Buf clip_read_text;

static size_t clip_max_bytes(void) {
  long units = XExtendedMaxRequestSize(clip_dpy);
  if (units == 0) units = XMaxRequestSize(clip_dpy);
  return (size_t)units * 4 - 1024;
}

static void clip_finish_read(bool ok) {
  clip_read_pending = false;
  clip_read_incr = false;
  clip_read_ok = ok;
  pthread_cond_broadcast(&clip_changed);
}

/// Appends the reply property to the text being read and deletes it. Returns its length in bytes.
static unsigned long clip_take_property(void) {
  Atom type;
  int format;
  unsigned long count = 0, after = 0;
  unsigned char *data = NULL;
  if (XGetWindowProperty(clip_dpy, clip_window, atom_property, 0, 0x1FFFFFFF, True, AnyPropertyType, &type, &format, &count, &after,
                         &data) != Success)
    return 0;
  if (type == atom_incr) {
    clip_read_incr = true;
    count = 1;
  } else if (data && format == 8) {
    buf_put(&clip_read_text, (const char *)data, count);
  }
  if (data) XFree(data);
  return count;
}

static void clip_serve(XSelectionRequestEvent *request) {
  XSelectionEvent reply = {.type = SelectionNotify, .display = request->display, .requestor = request->requestor,
                           .selection = request->selection, .target = request->target, .property = None, .time = request->time};
  Atom property = request->property != None ? request->property : request->target;
  pthread_mutex_lock(&clip_lock);
  if (clip_owner && request->selection == atom_clipboard) {
    if (request->target == atom_targets) {
      Atom targets[] = {atom_targets, atom_utf8, XA_STRING, atom_text};
      XChangeProperty(clip_dpy, request->requestor, property, XA_ATOM, 32, PropModeReplace, (unsigned char *)targets, 4);
      reply.property = property;
    } else if ((request->target == atom_utf8 || request->target == XA_STRING || request->target == atom_text) &&
               clip_text_length <= clip_max_bytes()) {
      Atom type = request->target == XA_STRING ? XA_STRING : atom_utf8;
      XChangeProperty(clip_dpy, request->requestor, property, type, 8, PropModeReplace, (unsigned char *)clip_text,
                      (int)clip_text_length);
      reply.property = property;
    }
  }
  pthread_mutex_unlock(&clip_lock);
  XSendEvent(clip_dpy, request->requestor, False, 0, (XEvent *)&reply);
  XFlush(clip_dpy);
}

static void clip_event(XEvent *event) {
  if (has_fixes && event->type == fixes_event_base + XFixesSelectionNotify) {
    XFixesSelectionNotifyEvent *notify = (XFixesSelectionNotifyEvent *)event;
    pthread_mutex_lock(&clip_lock);
    clip_change_count++;
    // The event may be older than this helper's latest copy, so ask the server who owns it now.
    if (notify->owner != clip_window) clip_owner = XGetSelectionOwner(clip_dpy, atom_clipboard) == clip_window;
    pthread_mutex_unlock(&clip_lock);
    return;
  }
  switch (event->type) {
    case SelectionRequest:
      clip_serve(&event->xselectionrequest);
      break;
    case SelectionClear:
      pthread_mutex_lock(&clip_lock);
      clip_owner = false;
      pthread_mutex_unlock(&clip_lock);
      break;
    case SelectionNotify:
      pthread_mutex_lock(&clip_lock);
      if (clip_read_pending && !clip_read_incr) {
        if (event->xselection.property == None) clip_finish_read(false);
        else {
          clip_take_property();
          if (!clip_read_incr) clip_finish_read(true);
        }
      }
      pthread_mutex_unlock(&clip_lock);
      break;
    case PropertyNotify:
      // Large copies arrive in pieces (INCR); an empty piece ends the transfer.
      pthread_mutex_lock(&clip_lock);
      if (clip_read_pending && clip_read_incr && event->xproperty.atom == atom_property &&
          event->xproperty.state == PropertyNewValue) {
        size_t before = clip_read_text.len;
        clip_take_property();
        if (clip_read_text.len == before) clip_finish_read(true);
      }
      pthread_mutex_unlock(&clip_lock);
      break;
  }
}

static void clip_command(char command) {
  if (command == 'S') {
    XSetSelectionOwner(clip_dpy, atom_clipboard, clip_window, CurrentTime);
    bool owner = XGetSelectionOwner(clip_dpy, atom_clipboard) == clip_window;
    pthread_mutex_lock(&clip_lock);
    clip_owner = owner;
    clip_set_ok = owner;
    clip_set_pending = false;
    pthread_cond_broadcast(&clip_changed);
    pthread_mutex_unlock(&clip_lock);
  } else if (command == 'R') {
    XConvertSelection(clip_dpy, atom_clipboard, atom_utf8, atom_property, clip_window, CurrentTime);
    XFlush(clip_dpy);
  }
}

static void *clip_thread(void *unused) {
  (void)unused;
  struct pollfd fds[2] = {{.fd = ConnectionNumber(clip_dpy), .events = POLLIN}, {.fd = clip_pipe[0], .events = POLLIN}};
  for (;;) {
    while (XPending(clip_dpy)) {
      XEvent event;
      XNextEvent(clip_dpy, &event);
      clip_event(&event);
    }
    if (poll(fds, 2, -1) < 0 && errno != EINTR) break;
    if (fds[1].revents & POLLIN) {
      char command;
      if (read(clip_pipe[0], &command, 1) == 1) clip_command(command);
    }
  }
  return NULL;
}

static bool clip_start(void) {
  if (clip_started) return true;
  clip_dpy = XOpenDisplay(NULL);
  if (!clip_dpy) return fail("could not open a clipboard connection to the X display");
  clip_window = XCreateSimpleWindow(clip_dpy, DefaultRootWindow(clip_dpy), -10, -10, 1, 1, 0, 0, 0);
  XSelectInput(clip_dpy, clip_window, PropertyChangeMask);
  atom_clipboard = XInternAtom(clip_dpy, "CLIPBOARD", False);
  atom_utf8 = XInternAtom(clip_dpy, "UTF8_STRING", False);
  atom_targets = XInternAtom(clip_dpy, "TARGETS", False);
  atom_text = XInternAtom(clip_dpy, "TEXT", False);
  atom_incr = XInternAtom(clip_dpy, "INCR", False);
  atom_property = XInternAtom(clip_dpy, "PASEO_SCREEN_CLIPBOARD", False);
  int error_base;
  if (has_fixes && XFixesQueryExtension(clip_dpy, &fixes_event_base, &error_base))
    XFixesSelectSelectionInput(clip_dpy, clip_window, atom_clipboard, XFixesSetSelectionOwnerNotifyMask);
  XFlush(clip_dpy);
  if (pipe(clip_pipe) != 0) return fail("could not create the clipboard pipe");
  pthread_t thread;
  if (pthread_create(&thread, NULL, clip_thread, NULL) != 0) return fail("could not start the clipboard thread");
  pthread_detach(thread);
  clip_started = true;
  return true;
}

static void deadline_after(struct timespec *deadline, long milliseconds) {
  clock_gettime(CLOCK_REALTIME, deadline);
  deadline->tv_sec += milliseconds / 1000;
  deadline->tv_nsec += (milliseconds % 1000) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_sec++;
    deadline->tv_nsec -= 1000000000L;
  }
}

/// Puts `text` on the host clipboard, owned by this helper.
static bool clip_set(const char *text, size_t length) {
  if (!clip_start()) return false;
  struct timespec deadline;
  deadline_after(&deadline, 2000);
  pthread_mutex_lock(&clip_lock);
  free(clip_text);
  clip_text = malloc(length + 1);
  if (!clip_text) abort();
  memcpy(clip_text, text, length);
  clip_text[length] = 0;
  clip_text_length = length;
  clip_set_pending = true;
  if (write(clip_pipe[1], "S", 1) != 1) clip_set_pending = false;
  while (clip_set_pending)
    if (pthread_cond_timedwait(&clip_changed, &clip_lock, &deadline) == ETIMEDOUT) break;
  bool ok = !clip_set_pending && clip_set_ok;
  clip_set_pending = false;
  pthread_mutex_unlock(&clip_lock);
  return ok ? true : fail("could not take ownership of the clipboard");
}

static bool read_clipboard(Buf *out) {
  if (!clip_start()) return false;
  pthread_mutex_lock(&clip_lock);
  if (clip_owner) {
    buf_printf(out, "\"changeCount\":%lu,\"text\":", clip_change_count);
    buf_json_string(out, clip_text, clip_text_length);
    pthread_mutex_unlock(&clip_lock);
    return true;
  }
  buf_free(&clip_read_text);
  buf_reserve(&clip_read_text, 0);
  clip_read_pending = true;
  clip_read_incr = false;
  clip_read_ok = false;
  struct timespec deadline;
  deadline_after(&deadline, 1500);
  if (write(clip_pipe[1], "R", 1) != 1) clip_read_pending = false;
  while (clip_read_pending)
    if (pthread_cond_timedwait(&clip_changed, &clip_lock, &deadline) == ETIMEDOUT) break;
  bool ok = !clip_read_pending && clip_read_ok;
  clip_read_pending = false;
  buf_printf(out, "\"changeCount\":%lu", clip_change_count);
  if (ok) {
    buf_puts(out, ",\"text\":");
    buf_json_string(out, clip_read_text.data, clip_read_text.len);
  }
  pthread_mutex_unlock(&clip_lock);
  return true;
}

#endif

// MARK: - Input

/// macOS virtual key code (Carbon kVK_*) to the X key code in the same physical position (evdev + 8).
static const unsigned char mac_to_x[128] = {
  [0] = 38, [1] = 39, [2] = 40, [3] = 41, [4] = 43, [5] = 42, [6] = 52, [7] = 53, [8] = 54, [9] = 55,
  [10] = 94, [11] = 56, [12] = 24, [13] = 25, [14] = 26, [15] = 27, [16] = 29, [17] = 28, [18] = 10, [19] = 11,
  [20] = 12, [21] = 13, [22] = 15, [23] = 14, [24] = 21, [25] = 18, [26] = 16, [27] = 20, [28] = 17, [29] = 19,
  [30] = 35, [31] = 32, [32] = 30, [33] = 34, [34] = 31, [35] = 33, [36] = 36, [37] = 46, [38] = 44, [39] = 48,
  [40] = 45, [41] = 47, [42] = 51, [43] = 59, [44] = 61, [45] = 57, [46] = 58, [47] = 60, [48] = 23, [49] = 65,
  [50] = 49, [51] = 22, [53] = 9, [54] = 134, [55] = 133, [56] = 50, [57] = 66, [58] = 64, [59] = 37,
  [60] = 62, [61] = 108, [62] = 105, [64] = 195, [65] = 91, [67] = 63, [69] = 86, [71] = 77, [72] = 123,
  [73] = 122, [74] = 121, [75] = 106, [76] = 104, [78] = 82, [79] = 196, [80] = 197, [81] = 125, [82] = 90,
  [83] = 87, [84] = 88, [85] = 89, [86] = 83, [87] = 84, [88] = 85, [89] = 79, [90] = 198, [91] = 80, [92] = 81,
  [93] = 132, [94] = 97, [95] = 103, [96] = 71, [97] = 72, [98] = 73, [99] = 69, [100] = 74, [101] = 75,
  [102] = 102, [103] = 95, [104] = 100, [105] = 191, [106] = 194, [107] = 192, [109] = 76, [110] = 135,
  [111] = 96, [113] = 193, [114] = 118, [115] = 110, [116] = 112, [117] = 119, [118] = 70, [119] = 115,
  [120] = 68, [121] = 117, [122] = 67, [123] = 113, [124] = 114, [125] = 116, [126] = 111,
};

#ifndef PASEO_WAYLAND
/// US ANSI positions (as macOS key codes) for letters a layout cannot produce.
static const signed char ansi_letters[26] = {
  0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6,
};
#endif

enum { MOD_CTRL = 1, MOD_ALT = 2, MOD_SHIFT = 4, MOD_SUPER = 8 };

#ifndef PASEO_WAYLAND
// Left-side modifier keys in press order, like the macOS helper.
static KeyCode modifier_keys[4];
static const int modifier_order[4] = {MOD_CTRL, MOD_ALT, MOD_SHIFT, MOD_SUPER};
static KeyCode scratch_keycode;  // an unused key code, remapped to type characters the layout lacks
static bool scratch_mapped;
#endif
static int last_pointer_x, last_pointer_y;
static bool has_last_pointer;
static double scroll_rest_x, scroll_rest_y;

static int modifier_mask(const Json *names) {
  int mask = 0;
  if (!names || names->type != J_ARR) return mask;
  for (size_t i = 0; i < names->count; i++) {
    const char *name = json_str(names->items[i]);
    if (!name) continue;
    if (!strcmp(name, "cmd") || !strcmp(name, "meta") || !strcmp(name, "super")) mask |= MOD_SUPER;
    else if (!strcmp(name, "shift")) mask |= MOD_SHIFT;
    else if (!strcmp(name, "alt") || !strcmp(name, "option")) mask |= MOD_ALT;
    else if (!strcmp(name, "ctrl") || !strcmp(name, "control")) mask |= MOD_CTRL;
  }
  return mask;
}

#ifndef PASEO_WAYLAND
static void press_modifiers(int mask, bool down) {
  for (int k = 0; k < 4; k++) {
    int i = down ? k : 3 - k;
    if ((mask & modifier_order[i]) && modifier_keys[i]) XTestFakeKeyEvent(dpy, modifier_keys[i], down, CurrentTime);
  }
}

static void tap_key(KeyCode code) {
  XTestFakeKeyEvent(dpy, code, True, CurrentTime);
  XTestFakeKeyEvent(dpy, code, False, CurrentTime);
}

static int current_group(void) {
  XkbStateRec state;
  return XkbGetState(dpy, XkbUseCoreKbd, &state) == Success ? state.group : 0;
}

static KeySym keysym_at(XkbDescPtr xkb, int code, int group, int level) {
  int groups = XkbKeyNumGroups(xkb, code);
  if (groups == 0) return NoSymbol;
  group %= groups;
  if (level >= XkbKeyGroupWidth(xkb, code, group)) return NoSymbol;
  return XkbKeySymEntry(xkb, code, level, group);
}

static bool is_keypad(KeySym sym) { return sym >= XK_KP_Space && sym <= XK_KP_Equal; }

/// Finds a key that types `sym` in `group` without AltGr. Sets `*shift` when it needs Shift.
static KeyCode find_key(XkbDescPtr xkb, KeySym sym, int group, bool *shift) {
  for (int code = xkb->min_key_code; code <= xkb->max_key_code; code++) {
    if (is_keypad(keysym_at(xkb, code, group, 0))) continue;
    for (int level = 0; level < 2; level++) {
      if (keysym_at(xkb, code, group, level) == sym) {
        if (shift) *shift = level == 1;
        return (KeyCode)code;
      }
    }
  }
  return 0;
}
#endif

static KeySym keysym_for(uint32_t cp) {
  if (cp == '\n' || cp == '\r') return XK_Return;
  if (cp == '\t') return XK_Tab;
  if (cp == '\b') return XK_BackSpace;
  if ((cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0xFF)) return cp;
  return 0x01000000 | cp;
}

#ifndef PASEO_WAYLAND
/// Key that types `character` on the host's layout, for shortcuts sent as a character (Ctrl+W on any
/// layout). Latin letters missing from the active layout come from another layout group, then from
/// their US position.
static KeyCode keycode_for_char(XkbDescPtr xkb, uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
  KeySym sym = keysym_for(cp);
  int group = current_group();
  KeyCode code = find_key(xkb, sym, group, NULL);
  for (int g = 0; !code && g < 4; g++)
    if (g != group) code = find_key(xkb, sym, g, NULL);
  if (!code && cp >= 'a' && cp <= 'z') code = mac_to_x[ansi_letters[cp - 'a']];
  return code;
}

/// Types one character: with its own key when the layout has it, else through the scratch key.
static void type_char(XkbDescPtr xkb, int group, uint32_t cp) {
  KeySym sym = keysym_for(cp);
  bool shift = false;
  KeyCode code = find_key(xkb, sym, group, &shift);
  if (code) {
    if (shift) XTestFakeKeyEvent(dpy, modifier_keys[2], True, CurrentTime);
    tap_key(code);
    if (shift) XTestFakeKeyEvent(dpy, modifier_keys[2], False, CurrentTime);
    return;
  }
  if (!scratch_keycode) return;
  KeySym syms[2] = {sym, sym};
  XChangeKeyboardMapping(dpy, scratch_keycode, 2, syms, 1);
  XSync(dpy, False);
  scratch_mapped = true;
  // Apps reload the keymap when they see the mapping change; give them a moment before the key.
  usleep(15000);
  tap_key(scratch_keycode);
  XSync(dpy, False);
  usleep(15000);
}

static void type_text(XkbDescPtr xkb, const char *text, size_t length) {
  const char *p = text, *end = text + length;
  int group = current_group();
  int typed = 0;
  while (p < end) {
    uint32_t cp = next_codepoint(&p, end);
    // Line breaks and tabs are real Return and Tab presses, as on macOS.
    if (cp == '\r' && p < end && *p == '\n') p++;
    if (cp == '\r' || cp == '\n') cp = '\n';
    type_char(xkb, group, cp);
    if (++typed % 32 == 0) XSync(dpy, False);
  }
  if (scratch_mapped) {
    // Let the app read the last remapped key before the mapping changes back under it.
    usleep(30000);
    KeySym none = NoSymbol;
    XChangeKeyboardMapping(dpy, scratch_keycode, 1, &none, 1);
    scratch_mapped = false;
  }
}
#endif

/// Maps normalized coordinates on `display` to the desktop. Values outside 0...1 (a drag that left the
/// viewed display) continue onto neighbouring displays and stop at the desktop's outer edge.
static void desktop_point(const Json *event, const Monitor *d, const Monitor *monitors, int count, int *out_x, int *out_y) {
  double x = json_num(json_get(event, "x"), 0.5), y = json_num(json_get(event, "y"), 0.5);
  double lx = d->x + x * (d->width - 1), ly = d->y + y * (d->height - 1);
  double best_x = lx, best_y = ly, best = INFINITY;
  for (int i = 0; i < count; i++) {
    const Monitor *m = &monitors[i];
    double cx = fmin(fmax(lx, m->x), m->x + m->width - 1);
    double cy = fmin(fmax(ly, m->y), m->y + m->height - 1);
    double distance = hypot(cx - lx, cy - ly);
    if (distance < best) {
      best = distance;
      best_x = cx;
      best_y = cy;
    }
  }
  *out_x = (int)lround(best_x);
  *out_y = (int)lround(best_y);
  last_pointer_x = *out_x;
  last_pointer_y = *out_y;
  has_last_pointer = true;
}

static unsigned int button_number(const Json *name) {
  const char *s = json_str(name);
  if (s && !strcmp(s, "right")) return 3;
  if (s && !strcmp(s, "middle")) return 2;
  return 1;
}

/// Scroll deltas are pixels; X11 and the portal scroll in wheel steps. Leftover pixels carry over to
/// the next event.
#define PIXELS_PER_STEP 50.0

#ifndef PASEO_WAYLAND
static void scroll_steps(double *rest, double delta, unsigned int negative, unsigned int positive) {
  *rest += delta;
  int steps = (int)(*rest / PIXELS_PER_STEP);
  *rest -= steps * PIXELS_PER_STEP;
  if (steps > 100) steps = 100;
  if (steps < -100) steps = -100;
  unsigned int button = steps > 0 ? positive : negative;
  for (int i = 0; i < abs(steps); i++) {
    XTestFakeButtonEvent(dpy, button, True, CurrentTime);
    XTestFakeButtonEvent(dpy, button, False, CurrentTime);
  }
}

static bool handle_input(const Json *request, Buf *out) {
  if (!has_xtest) return fail("The X server has no XTEST extension, so input cannot be sent");
  // Input is an explicit action, so it wakes a sleeping display before it lands.
  if (displays_asleep()) wake_displays();
  Monitor monitors[MAX_MONITORS];
  int count = list_monitors(monitors);
  const Monitor *d = resolve_monitor(monitors, count, json_get(request, "display"));
  const Json *events = json_get(request, "events");
  size_t handled = events && events->type == J_ARR ? events->count : 0;
  // Read fresh, so a layout switched on the host is respected.
  XkbDescPtr xkb = XkbGetMap(dpy, XkbKeyTypesMask | XkbKeySymsMask, XkbUseCoreKbd);
  if (!xkb) return fail("could not read the keyboard layout");
  bool ok = true;
  for (size_t i = 0; ok && i < handled; i++) {
    const Json *event = events->items[i];
    const char *type = json_str(json_get(event, "type"));
    int mods = modifier_mask(json_get(event, "modifiers"));
    int x, y;
    if (!type) {
      ok = fail("input event without a type");
    } else if (!strcmp(type, "move")) {
      desktop_point(event, d, monitors, count, &x, &y);
      XTestFakeMotionEvent(dpy, -1, x, y, CurrentTime);
    } else if (!strcmp(type, "down") || !strcmp(type, "up")) {
      desktop_point(event, d, monitors, count, &x, &y);
      XTestFakeMotionEvent(dpy, -1, x, y, CurrentTime);
      press_modifiers(mods, true);
      XTestFakeButtonEvent(dpy, button_number(json_get(event, "button")), !strcmp(type, "down"), CurrentTime);
      press_modifiers(mods, false);
    } else if (!strcmp(type, "click")) {
      desktop_point(event, d, monitors, count, &x, &y);
      XTestFakeMotionEvent(dpy, -1, x, y, CurrentTime);
      unsigned int button = button_number(json_get(event, "button"));
      int clicks = (int)json_num(json_get(event, "count"), 1);
      if (clicks < 1) clicks = 1;
      if (clicks > 3) clicks = 3;
      press_modifiers(mods, true);
      for (int c = 0; c < clicks; c++) {
        XTestFakeButtonEvent(dpy, button, True, CurrentTime);
        XTestFakeButtonEvent(dpy, button, False, CurrentTime);
      }
      press_modifiers(mods, false);
    } else if (!strcmp(type, "scroll")) {
      if (json_get(event, "x")) {
        desktop_point(event, d, monitors, count, &x, &y);
        XTestFakeMotionEvent(dpy, -1, x, y, CurrentTime);
      }
      double dx = fmin(10000, fmax(-10000, json_num(json_get(event, "dx"), 0)));
      double dy = fmin(10000, fmax(-10000, json_num(json_get(event, "dy"), 0)));
      // Buttons 4/5 scroll up/down and 6/7 left/right; a positive delta scrolls down or right.
      scroll_steps(&scroll_rest_y, dy, 4, 5);
      scroll_steps(&scroll_rest_x, dx, 6, 7);
    } else if (!strcmp(type, "key")) {
      // Either a physical key code, or a character resolved on the host's layout.
      const char *chars = json_str(json_get(event, "char"));
      KeyCode code = 0;
      if (chars && *chars) {
        const char *p = chars;
        code = keycode_for_char(xkb, next_codepoint(&p, chars + strlen(chars)));
      }
      double key = json_num(json_get(event, "keyCode"), -1);
      if (!code && key >= 0 && key < 128) code = mac_to_x[(int)key];
      if (!code) {
        ok = fail("no key for %s on this keyboard layout", chars ? chars : "this key code");
        break;
      }
      const Json *down = json_get(event, "down");
      press_modifiers(mods, true);
      if (down && down->type == J_BOOL) {
        XTestFakeKeyEvent(dpy, code, down->num != 0, CurrentTime);
      } else {
        tap_key(code);
      }
      press_modifiers(mods, false);
    } else if (!strcmp(type, "text")) {
      const Json *text = json_get(event, "text");
      if (text && text->type == J_STR) type_text(xkb, text->str, text->len);
    } else if (!strcmp(type, "paste")) {
      // Puts the text on the host clipboard and presses Ctrl+V (Ctrl+Shift+V with shift, for terminals).
      const Json *text = json_get(event, "text");
      if (!clip_set(text && text->type == J_STR ? text->str : "", text && text->type == J_STR ? text->len : 0)) {
        ok = false;
        break;
      }
      usleep(30000);
      KeyCode v = keycode_for_char(xkb, 'v');
      int paste_mods = MOD_CTRL | (json_bool(json_get(event, "shift"), false) ? MOD_SHIFT : 0);
      press_modifiers(paste_mods, true);
      tap_key(v);
      press_modifiers(paste_mods, false);
    } else {
      ok = fail("unknown input event: %s", type);
    }
    XFlush(dpy);
  }
  XkbFreeKeyboard(xkb, 0, True);
  XSync(dpy, False);
  if (!ok) return false;
  buf_printf(out, "\"handled\":%zu", handled);
  // Tells the client which display the pointer ended on, so the view can follow a drag to another display.
  if (has_last_pointer) {
    const Monitor *landed = monitor_containing(monitors, count, last_pointer_x, last_pointer_y);
    if (landed) buf_printf(out, ",\"pointerDisplay\":%lu", landed->id);
  }
  return true;
}

// MARK: - Status

/// The display showing the centre of the focused window (_NET_ACTIVE_WINDOW), or NULL.
static const Monitor *active_monitor(const Monitor *monitors, int count) {
  Atom property = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
  if (property == None) return NULL;
  Atom type;
  int format;
  unsigned long items, after;
  unsigned char *data = NULL;
  if (XGetWindowProperty(dpy, root, property, 0, 1, False, XA_WINDOW, &type, &format, &items, &after, &data) != Success || !data)
    return NULL;
  Window window = type == XA_WINDOW && format == 32 && items == 1 ? (Window)((unsigned long *)data)[0] : None;
  XFree(data);
  if (window == None) return NULL;
  // The window may close between the two calls; its X error is recorded instead of ending the helper.
  main_x_error = false;
  XWindowAttributes attributes;
  int x, y;
  Window child;
  if (!XGetWindowAttributes(dpy, window, &attributes) || !XTranslateCoordinates(dpy, window, root, 0, 0, &x, &y, &child) ||
      main_x_error)
    return NULL;
  return monitor_containing(monitors, count, x + attributes.width / 2, y + attributes.height / 2);
}

static void status(Buf *out) {
  Monitor monitors[MAX_MONITORS];
  int count = list_monitors(monitors);
  // X11 has no capture or input permissions; input only needs the XTEST extension.
  buf_printf(out, "\"screenRecording\":true,\"accessibility\":%s,\"locked\":%s,\"displays\":[", has_xtest ? "true" : "false",
             screen_locked() ? "true" : "false");
  for (int i = 0; i < count; i++) {
    if (i) buf_puts(out, ",");
    monitor_json(out, &monitors[i]);
  }
  buf_puts(out, "]");
  const Monitor *active = active_monitor(monitors, count);
  if (active) buf_printf(out, ",\"activeDisplay\":%lu", active->id);
}

/// X11 has nothing to ask for, so a permission request is a status read.
static bool request_permissions(Buf *out) {
  status(out);
  return true;
}

#endif

#ifdef PASEO_WAYLAND
// MARK: - Wayland: portal session
//
// On Wayland nothing can read the screen or send input without the compositor's consent. The
// helper asks through xdg-desktop-portal: one RemoteDesktop session with a ScreenCast of every
// monitor and keyboard and pointer access. The first Start shows the desktop's own dialog on the
// host; the restore token it returns is saved, so later sessions start without asking until the
// user revokes it. Frames arrive over PipeWire through GStreamer; input goes through Notify* calls.

#define PORTAL_BUS "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_REMOTE "org.freedesktop.portal.RemoteDesktop"
#define PORTAL_SCREENCAST "org.freedesktop.portal.ScreenCast"
#define PORTAL_CLIPBOARD "org.freedesktop.portal.Clipboard"
#define PORTAL_SESSION "org.freedesktop.portal.Session"

enum { DEVICE_KEYBOARD = 1, DEVICE_POINTER = 2 };
enum { SESSION_NONE, SESSION_STARTING, SESSION_READY };

// Linux input event codes (linux/input-event-codes.h).
enum { EV_KEY_ENTER = 28, EV_KEY_TAB = 15, EV_KEY_LEFTCTRL = 29, EV_KEY_LEFTALT = 56, EV_KEY_LEFTSHIFT = 42, EV_KEY_LEFTMETA = 125 };
enum { EV_BTN_LEFT = 0x110, EV_BTN_RIGHT = 0x111, EV_BTN_MIDDLE = 0x112 };

typedef struct {
  uint32_t node;
  Monitor monitor;  // logical position and size, as input coordinates use them
  GstElement *pipeline;
  GstElement *sink;
  GstSample *sample;  // the latest frame
} Stream;

typedef struct Waiter Waiter;

static GDBusConnection *bus;
static GMainLoop *loop;
// Guards everything below except the clipboard. The start thread owns what it opens until it
// publishes a working session here; the request threads use and tear down the published one, and
// the main thread only flags closures.
static GMutex session_lock;
static int session_state = SESSION_NONE;
static unsigned session_id;     // the start attempt that produced the published session
static bool session_closed;     // the desktop closed the published session (the host stopped sharing)
static char *session_handle;
static uint32_t session_devices;
static Stream streams[MAX_MONITORS];
static int stream_count;
static int pipewire_fd = -1;
static char session_error[512];
static guint closed_subscription;
static unsigned attempt_id;     // the latest start attempt
static bool start_running;      // a start thread is alive; it may outlive SESSION_STARTING by its cleanup
static bool start_closed;       // the desktop closed the session the start thread is setting up
static Waiter *start_waiter;    // the portal reply the start thread is waiting for
static bool auto_start = true;  // whether a saved token may start a session without a request
static char token_file[1024];

static GMutex clip_lock;
static bool clipboard_enabled;
static unsigned long clip_changes;
static char **clip_offered;  // MIME types another app offers
static char *clip_text;      // text this helper put on the clipboard
static size_t clip_text_length;
static bool clip_owner;

static unsigned token_counter;
static bool mutter;  // the compositor is GNOME's Mutter (its DisplayConfig answered)

static char *new_token(void) { return g_strdup_printf("paseo%u_%u", (unsigned)getpid(), ++token_counter); }

/// Portal replies arrive as a Response signal on a Request object whose path is derived from our
/// unique bus name and the handle_token we chose.
static char *request_path(const char *token) {
  char *sender = g_strdup(g_dbus_connection_get_unique_name(bus) + 1);
  for (char *p = sender; *p; p++)
    if (*p == '.') *p = '_';
  char *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
  g_free(sender);
  return path;
}

struct Waiter {
  GMutex lock;
  GCond cond;
  bool done;
  bool released;  // the Response subscription will not call back any more
  uint32_t response;
  GVariant *results;
};

/// Ends a wait with `response`, unless the reply already came. `results` is taken.
static void finish_wait(Waiter *waiter, uint32_t response, GVariant *results) {
  g_mutex_lock(&waiter->lock);
  if (!waiter->done) {
    waiter->response = response;
    waiter->results = results;
    results = NULL;
    waiter->done = true;
    g_cond_signal(&waiter->cond);
  }
  g_mutex_unlock(&waiter->lock);
  if (results) g_variant_unref(results);
}

static void on_response(GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                        const char *signal, GVariant *parameters, gpointer data) {
  uint32_t response;
  GVariant *results;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);
  finish_wait(data, response, results);
}

static void on_response_released(gpointer data) {
  Waiter *waiter = data;
  g_mutex_lock(&waiter->lock);
  waiter->released = true;
  g_cond_signal(&waiter->cond);
  g_mutex_unlock(&waiter->lock);
}

/// Calls a portal method that answers through a Request and waits for the Response. Only the start
/// thread calls it (the Response is delivered on the main thread). Returns 0 on success, 1 when the
/// user cancelled, 2 for other portal failures (including the session closing meanwhile), -1 when
/// the call itself failed, -2 on timeout. `results` is set on success only; `error` gets a message
/// for -1.
static int portal_call(const char *interface, const char *method, GVariant *parameters, const char *token,
                       gint64 timeout_us, GVariant **results, char *error_text, size_t error_size) {
  *results = NULL;
  char *path = request_path(token);
  Waiter waiter = {0};
  g_mutex_init(&waiter.lock);
  g_cond_init(&waiter.cond);
  guint subscription = g_dbus_connection_signal_subscribe(bus, PORTAL_BUS, "org.freedesktop.portal.Request", "Response", path,
                                                          NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_response, &waiter, on_response_released);
  // A session closed by the desktop answers no more requests, so its closing ends the wait.
  g_mutex_lock(&session_lock);
  start_waiter = &waiter;
  if (start_closed) finish_wait(&waiter, 2, NULL);
  g_mutex_unlock(&session_lock);
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(bus, PORTAL_BUS, PORTAL_PATH, interface, method, parameters, NULL,
                                                G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);
  if (!reply) {
    snprintf(error_text, error_size, "%s.%s failed: %s", interface, method, error->message);
    g_error_free(error);
  } else {
    g_variant_unref(reply);
    gint64 deadline = g_get_monotonic_time() + timeout_us;
    g_mutex_lock(&waiter.lock);
    while (!waiter.done)
      if (!g_cond_wait_until(&waiter.cond, &waiter.lock, deadline)) break;
    g_mutex_unlock(&waiter.lock);
  }
  g_mutex_lock(&session_lock);
  start_waiter = NULL;
  g_mutex_unlock(&session_lock);
  // A Response already queued on the main thread may still arrive after unsubscribing; the waiter
  // lives on this stack, so wait until GDBus lets go of it.
  g_dbus_connection_signal_unsubscribe(bus, subscription);
  gint64 release_deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  g_mutex_lock(&waiter.lock);
  while (!waiter.released)
    if (!g_cond_wait_until(&waiter.cond, &waiter.lock, release_deadline)) break;
  int code = !reply ? -1 : waiter.done ? (int)waiter.response : -2;
  g_mutex_unlock(&waiter.lock);
  if (code == 0) *results = waiter.results;
  else if (waiter.results) g_variant_unref(waiter.results);
  g_mutex_clear(&waiter.lock);
  g_cond_clear(&waiter.cond);
  g_free(path);
  return code;
}

static GVariant *token_options(const char *token, GVariantBuilder *extra) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(token));
  if (extra) {
    GVariant *more = g_variant_builder_end(extra);
    GVariantIter iter;
    const char *key;
    GVariant *value;
    g_variant_iter_init(&iter, more);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
      g_variant_builder_add(&builder, "{sv}", key, value);
      g_variant_unref(value);
    }
    g_variant_unref(g_variant_ref_sink(more));
  }
  return g_variant_builder_end(&builder);
}

static char *read_restore_token(void) {
  char *contents = NULL;
  if (!g_file_get_contents(token_file, &contents, NULL, NULL)) return NULL;
  g_strstrip(contents);
  if (!*contents) {
    g_free(contents);
    return NULL;
  }
  return contents;
}

static void save_restore_token(const char *token) {
  char *dir = g_path_get_dirname(token_file);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);
  // Created private, rather than made private after the write.
  g_file_set_contents_full(token_file, token, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
}

// MARK: - Wayland: display names (GNOME)

/// Names monitors and finds the primary one from Mutter's DisplayConfig, matching portal streams
/// by their logical position. Other desktops keep the generic names.
static void name_monitors(void) {
  GVariant *state = g_dbus_connection_call_sync(bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                                                "org.gnome.Mutter.DisplayConfig", "GetCurrentState", NULL, NULL,
                                                G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
  if (!state) return;
  mutter = true;
  GVariant *monitors = g_variant_get_child_value(state, 1);
  GVariant *logical = g_variant_get_child_value(state, 2);
  GVariantIter iter;
  GVariant *lm;
  g_variant_iter_init(&iter, logical);
  while ((lm = g_variant_iter_next_value(&iter))) {
    gint32 x, y;
    gboolean primary;
    GVariant *specs;
    g_variant_get_child(lm, 0, "i", &x);
    g_variant_get_child(lm, 1, "i", &y);
    g_variant_get_child(lm, 4, "b", &primary);
    specs = g_variant_get_child_value(lm, 5);
    const char *connector = NULL;
    if (g_variant_n_children(specs) > 0) g_variant_get_child(specs, 0, "(&s&s&s&s)", &connector, NULL, NULL, NULL);
    for (int i = 0; i < stream_count; i++) {
      Monitor *m = &streams[i].monitor;
      if (m->x != x || m->y != y) continue;
      m->main = primary;
      GVariantIter miter;
      GVariant *monitor;
      g_variant_iter_init(&miter, monitors);
      while (connector && (monitor = g_variant_iter_next_value(&miter))) {
        const char *mc;
        g_variant_get_child(monitor, 0, "(&s&s&s&s)", &mc, NULL, NULL, NULL);
        if (!strcmp(mc, connector)) {
          GVariant *props = g_variant_get_child_value(monitor, 2);
          const char *display_name = NULL;
          gboolean builtin = FALSE;
          g_variant_lookup(props, "display-name", "&s", &display_name);
          g_variant_lookup(props, "is-builtin", "b", &builtin);
          m->builtin = builtin;
          snprintf(m->name, sizeof m->name, "%s", builtin ? "Built-in Display" : display_name ? display_name : connector);
          g_variant_unref(props);
        }
        g_variant_unref(monitor);
      }
    }
    g_variant_unref(specs);
    g_variant_unref(lm);
  }
  g_variant_unref(logical);
  g_variant_unref(monitors);
  g_variant_unref(state);
}

// MARK: - Wayland: session lifecycle

static void on_session_closed(GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                              const char *signal, GVariant *parameters, gpointer data) {
  // Only flagged here. The request threads tear down a published session; a session still being
  // set up is closed by its start thread, which this wakes if it is waiting on the portal.
  unsigned id = GPOINTER_TO_UINT(data);
  g_mutex_lock(&session_lock);
  if (id == session_id) session_closed = true;
  if (id == attempt_id && start_running) {
    start_closed = true;
    if (start_waiter) finish_wait(start_waiter, 2, NULL);
  }
  g_mutex_unlock(&session_lock);
}

static void close_portal_session(const char *handle) {
  GVariant *reply = g_dbus_connection_call_sync(bus, PORTAL_BUS, handle, PORTAL_SESSION, "Close", NULL, NULL,
                                                G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
  if (reply) g_variant_unref(reply);
}

static void stop_streams(Stream *list, int count) {
  for (int i = 0; i < count; i++) {
    if (list[i].pipeline) {
      gst_element_set_state(list[i].pipeline, GST_STATE_NULL);
      gst_object_unref(list[i].pipeline);
    }
    if (list[i].sink) gst_object_unref(list[i].sink);
    if (list[i].sample) gst_sample_unref(list[i].sample);
  }
  memset(list, 0, sizeof *list * (size_t)count);
}

/// Releases the published session. Called with session_lock held.
static void teardown_session(void) {
  stop_streams(streams, stream_count);
  stream_count = 0;
  if (pipewire_fd >= 0) close(pipewire_fd);
  pipewire_fd = -1;
  if (closed_subscription) g_dbus_connection_signal_unsubscribe(bus, closed_subscription);
  closed_subscription = 0;
  if (session_handle && !session_closed) close_portal_session(session_handle);
  g_free(session_handle);
  session_handle = NULL;
  session_closed = false;
  session_id = 0;
  session_devices = 0;
  session_state = SESSION_NONE;
  g_mutex_lock(&clip_lock);
  clipboard_enabled = false;
  clip_owner = false;
  g_mutex_unlock(&clip_lock);
}

static bool pipewire_available(void) {
  GstElementFactory *source = gst_element_factory_find("pipewiresrc");
  if (source) gst_object_unref(source);
  return source != NULL;
}

/// Starts a GStreamer pipeline per stream. pipewiresrc duplicates `fd` for itself, so the caller
/// keeps (and later closes) its own.
static bool start_pipelines(Stream *list, int count, int fd, char *error, size_t error_size) {
  for (int i = 0; i < count; i++) {
    Stream *s = &list[i];
    char description[512];
    // `path` takes the portal's node id (`target-object` wants a serial or name, and a miss there
    // silently links the default video source, a webcam). dont-fallback and dont-reconnect make a
    // missing target an error instead of a link to some other camera or screen. keepalive-time
    // resends the last frame now and then, so a still screen still has a frame to read.
    snprintf(description, sizeof description,
             "pipewiresrc fd=%d path=%u always-copy=true keepalive-time=1000 "
             "stream-properties=\"props,node.dont-fallback=(boolean)true,node.dont-reconnect=(boolean)true\" ! "
             "videoconvert ! video/x-raw,format=BGRx ! appsink name=sink max-buffers=1 drop=true sync=false",
             fd, s->node);
    GError *failure = NULL;
    s->pipeline = gst_parse_launch(description, &failure);
    if (!s->pipeline) {
      snprintf(error, error_size, "could not start the screen stream: %s", failure ? failure->message : "?");
      if (failure) g_error_free(failure);
      return false;
    }
    s->sink = gst_bin_get_by_name(GST_BIN(s->pipeline), "sink");
    if (!s->sink || gst_element_set_state(s->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      snprintf(error, error_size, "could not start the screen stream");
      return false;
    }
  }
  return true;
}

static uint32_t stable_id(const char *text) {
  uint32_t hash = 2166136261u;
  for (const char *p = text; *p; p++) hash = (hash ^ (uint8_t)*p) * 16777619u;
  return (hash & 0x7FFFFFFF) | 1;
}

/// Reads the streams the host shared from Start's results.
static int read_streams(GVariant *results, Stream *out) {
  int count = 0;
  GVariant *list = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
  if (!list) return 0;
  GVariantIter iter;
  uint32_t node;
  GVariant *props;
  g_variant_iter_init(&iter, list);
  while (count < MAX_MONITORS && g_variant_iter_next(&iter, "(u@a{sv})", &node, &props)) {
    Stream *s = &out[count];
    memset(s, 0, sizeof *s);
    s->node = node;
    gint32 x = 0, y = 0, w = 0, h = 0;
    g_variant_lookup(props, "position", "(ii)", &x, &y);
    g_variant_lookup(props, "size", "(ii)", &w, &h);
    const char *id = NULL;
    g_variant_lookup(props, "id", "&s", &id);
    char fallback[32];
    snprintf(fallback, sizeof fallback, "%d,%d", x, y);
    s->monitor = (Monitor){.id = stable_id(id ? id : fallback), .x = x, .y = y, .width = w, .height = h};
    snprintf(s->monitor.name, sizeof s->monitor.name, "Display %d", count + 1);
    count++;
    g_variant_unref(props);
  }
  g_variant_unref(list);
  return count;
}

/// Runs the portal handshake on its own thread, because Start waits for someone at the host to
/// answer the dialog. Status polling reports "awaiting approval" meanwhile. Everything opened here
/// stays this thread's until a working session is published, and is closed again on any failure.
static gpointer start_session(gpointer data) {
  const unsigned id = GPOINTER_TO_UINT(data);
  char *saved = read_restore_token();
  char *handle = NULL;
  guint subscription = 0;
  int fd = -1;
  Stream found[MAX_MONITORS];
  int found_count = 0;
  GVariant *results = NULL;
  uint32_t devices = 0;
  gboolean clipboard = FALSE;
  bool presented = false;  // the saved token went to the portal (CreateSession fails without it)
  bool started = false;    // Start succeeded, which used up the saved token
  bool published = false;
  char error[512] = "";
  int code = 3;  // 3: failed on this side rather than in the portal
  char *token, *session_token;
  GVariantBuilder extra;

  memset(found, 0, sizeof found);
  // Checked first, so a host without the plugin is not asked to approve a session that cannot work.
  if (!pipewire_available()) {
    snprintf(error, sizeof error,
             "GStreamer's PipeWire plugin is missing, so the screen cannot be read. Install it on the host "
             "(Debian or Ubuntu: gstreamer1.0-pipewire; Fedora: pipewire-gstreamer; Arch: gst-plugin-pipewire).");
    goto done;
  }

  token = new_token();
  session_token = new_token();
  g_variant_builder_init(&extra, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&extra, "{sv}", "session_handle_token", g_variant_new_string(session_token));
  code = portal_call(PORTAL_REMOTE, "CreateSession", g_variant_new("(@a{sv})", token_options(token, &extra)), token,
                     30 * G_USEC_PER_SEC, &results, error, sizeof error);
  g_free(token);
  g_free(session_token);
  if (code != 0) goto done;
  g_variant_lookup(results, "session_handle", "s", &handle);
  g_variant_unref(results);
  results = NULL;
  if (!handle) {
    code = 2;
    goto done;
  }
  subscription = g_dbus_connection_signal_subscribe(bus, PORTAL_BUS, PORTAL_SESSION, "Closed", handle, NULL,
                                                    G_DBUS_SIGNAL_FLAGS_NONE, on_session_closed, GUINT_TO_POINTER(id), NULL);

  // Clipboard access has to be requested before Start; desktops without the portal just skip it.
  GVariant *clip = g_dbus_connection_call_sync(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_CLIPBOARD, "RequestClipboard",
                                               g_variant_new("(oa{sv})", handle, NULL), NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
                                               NULL, NULL);
  if (clip) g_variant_unref(clip);

  presented = saved != NULL;
  token = new_token();
  g_variant_builder_init(&extra, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&extra, "{sv}", "types", g_variant_new_uint32(DEVICE_KEYBOARD | DEVICE_POINTER));
  g_variant_builder_add(&extra, "{sv}", "persist_mode", g_variant_new_uint32(2));  // until revoked
  if (saved) g_variant_builder_add(&extra, "{sv}", "restore_token", g_variant_new_string(saved));
  code = portal_call(PORTAL_REMOTE, "SelectDevices", g_variant_new("(o@a{sv})", handle, token_options(token, &extra)), token,
                     30 * G_USEC_PER_SEC, &results, error, sizeof error);
  g_free(token);
  if (results) g_variant_unref(results);
  results = NULL;
  if (code != 0) goto done;

  token = new_token();
  g_variant_builder_init(&extra, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&extra, "{sv}", "types", g_variant_new_uint32(1));       // monitors
  g_variant_builder_add(&extra, "{sv}", "multiple", g_variant_new_boolean(TRUE));
  g_variant_builder_add(&extra, "{sv}", "cursor_mode", g_variant_new_uint32(2));  // pointer drawn into frames
  code = portal_call(PORTAL_SCREENCAST, "SelectSources", g_variant_new("(o@a{sv})", handle, token_options(token, &extra)),
                     token, 30 * G_USEC_PER_SEC, &results, error, sizeof error);
  g_free(token);
  if (results) g_variant_unref(results);
  results = NULL;
  if (code != 0) goto done;

  // The dialog, when there is one, waits for a person at the host.
  token = new_token();
  code = portal_call(PORTAL_REMOTE, "Start", g_variant_new("(os@a{sv})", handle, "", token_options(token, NULL)), token,
                     10 * 60 * G_USEC_PER_SEC, &results, error, sizeof error);
  g_free(token);
  if (code != 0) goto done;
  started = true;
  const char *restore = NULL;
  if (g_variant_lookup(results, "restore_token", "&s", &restore)) save_restore_token(restore);
  g_variant_lookup(results, "devices", "u", &devices);
  g_variant_lookup(results, "clipboard_enabled", "b", &clipboard);
  code = 3;

  GUnixFDList *fds = NULL;
  GVariant *remote = g_dbus_connection_call_with_unix_fd_list_sync(
      bus, PORTAL_BUS, PORTAL_PATH, PORTAL_SCREENCAST, "OpenPipeWireRemote", g_variant_new("(oa{sv})", handle, NULL),
      G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &fds, NULL, NULL);
  if (remote) {
    gint32 fd_index;
    g_variant_get(remote, "(h)", &fd_index);
    fd = g_unix_fd_list_get(fds, fd_index, NULL);
    g_variant_unref(remote);
  }
  if (fds) g_object_unref(fds);
  if (fd < 0) {
    snprintf(error, sizeof error, "could not open the PipeWire stream");
    goto done;
  }

  found_count = read_streams(results, found);
  if (!found_count) {
    snprintf(error, sizeof error, "The host shared no screens.");
    goto done;
  }
  if (!start_pipelines(found, found_count, fd, error, sizeof error)) goto done;

  g_mutex_lock(&session_lock);
  if (start_closed) {
    g_mutex_unlock(&session_lock);
    code = 2;
    goto done;
  }
  session_handle = handle;
  handle = NULL;
  closed_subscription = subscription;
  subscription = 0;
  pipewire_fd = fd;
  fd = -1;
  memcpy(streams, found, sizeof found);
  stream_count = found_count;
  memset(found, 0, sizeof found);
  found_count = 0;
  session_devices = devices;
  session_id = id;
  session_closed = false;
  session_state = SESSION_READY;
  session_error[0] = 0;
  start_running = false;
  name_monitors();
  bool any_main = false;
  for (int i = 0; i < stream_count; i++) any_main |= streams[i].monitor.main;
  if (!any_main) streams[0].monitor.main = true;
  g_mutex_unlock(&session_lock);
  g_mutex_lock(&clip_lock);
  clipboard_enabled = clipboard;
  g_mutex_unlock(&clip_lock);
  published = true;

done:
  if (!published) {
    // GNOME refuses new sessions while the screen is locked; that says nothing about the token.
    bool locked = code != 0 && code != 3 && screen_locked();
    g_mutex_lock(&session_lock);
    if (start_closed) snprintf(session_error, sizeof session_error, "Screen sharing was stopped on the host.");
    else if (locked) snprintf(session_error, sizeof session_error, "The host's screen is locked. Unlock it, then try again.");
    else if (code == 1) snprintf(session_error, sizeof session_error, "Screen sharing was declined on the host.");
    else if (code == -2) snprintf(session_error, sizeof session_error, "Nobody answered the screen sharing prompt on the host.");
    else if (error[0]) snprintf(session_error, sizeof session_error, "%s", error);
    else snprintf(session_error, sizeof session_error, "The desktop refused screen sharing (portal error %d).", code);
    // A token the desktop no longer accepts would fail the same way every time.
    if (presented && !started && !locked && !start_closed && (code == 1 || code == 2 || code == -2)) unlink(token_file);
    session_state = SESSION_NONE;
    start_running = false;
    g_mutex_unlock(&session_lock);
  }
  stop_streams(found, found_count);
  if (fd >= 0) close(fd);
  if (subscription) g_dbus_connection_signal_unsubscribe(bus, subscription);
  if (handle) close_portal_session(handle);
  g_free(handle);
  if (results) g_variant_unref(results);
  g_free(saved);
  return NULL;
}

/// Starts the handshake unless a session is running or one is being set up or cleaned up. Called
/// with session_lock held.
static void begin_session(void) {
  if (session_state != SESSION_NONE || start_running) return;
  session_state = SESSION_STARTING;
  session_error[0] = 0;
  start_running = true;
  start_closed = false;
  g_thread_unref(g_thread_new("portal-start", start_session, GUINT_TO_POINTER(++attempt_id)));
}

/// Brings the session up to date before a request. Called with session_lock held.
static void refresh_session(void) {
  if (session_closed) {
    teardown_session();
    // The host stopped sharing (from the desktop's sharing indicator, say). Sharing resumes only
    // once someone there approves again, so the token that would skip the dialog is dropped.
    unlink(token_file);
    snprintf(session_error, sizeof session_error, "Screen sharing was stopped on the host.");
  }
  // A helper's first request resumes an approved session silently. After that, sessions start only
  // when asked for, so a failure is not retried in the background.
  if (auto_start && g_file_test(token_file, G_FILE_TEST_EXISTS)) begin_session();
  auto_start = false;
}

/// Waits up to `seconds` for a session that is starting. Called with session_lock held; releases
/// it while waiting.
static void wait_for_session(double seconds) {
  double until = now_seconds() + seconds;
  while (session_state == SESSION_STARTING && now_seconds() < until) {
    g_mutex_unlock(&session_lock);
    usleep(50000);
    g_mutex_lock(&session_lock);
  }
}

static int monitors_of_streams(Monitor *out) {
  for (int i = 0; i < stream_count; i++) out[i] = streams[i].monitor;
  return stream_count;
}

// MARK: - Wayland: sleep

/// GNOME's display power state: 0 on, 1 standby, 2 suspend, 3 off; -1 when unknown.
static int power_save_mode(void) {
  GVariant *reply = g_dbus_connection_call_sync(bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                                                "org.freedesktop.DBus.Properties", "Get",
                                                g_variant_new("(ss)", "org.gnome.Mutter.DisplayConfig", "PowerSaveMode"),
                                                G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  if (!reply) return -1;
  GVariant *value;
  g_variant_get(reply, "(v)", &value);
  int mode = g_variant_is_of_type(value, G_VARIANT_TYPE_INT32) ? g_variant_get_int32(value) : -1;
  g_variant_unref(value);
  g_variant_unref(reply);
  return mode;
}

static bool displays_asleep(void) { return power_save_mode() > 0; }

static void notify(const char *method, GVariant *parameters);

/// Wakes the displays the way user input would. Called with session_lock held.
static void wake_displays(void) {
  g_dbus_connection_call_sync(bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                              "org.freedesktop.DBus.Properties", "Set",
                              g_variant_new("(ssv)", "org.gnome.Mutter.DisplayConfig", "PowerSaveMode", g_variant_new_int32(0)),
                              NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  if (session_state == SESSION_READY && (session_devices & DEVICE_POINTER)) {
    notify("NotifyPointerMotion", g_variant_new("(oa{sv}dd)", session_handle, NULL, 1.0, 0.0));
    notify("NotifyPointerMotion", g_variant_new("(oa{sv}dd)", session_handle, NULL, -1.0, 0.0));
  }
  for (int i = 0; i < 30 && displays_asleep(); i++) usleep(100000);
}

// MARK: - Wayland: capture

static bool capture_frame(const Json *request, Buf *out) {
  g_mutex_lock(&session_lock);
  refresh_session();
  wait_for_session(3);
  if (session_state != SESSION_READY) {
    g_mutex_unlock(&session_lock);
    return fail("%s", session_error[0] ? session_error : "Screen sharing has not been approved on the host yet.");
  }
  if (displays_asleep()) {
    if (!json_bool(json_get(request, "wake"), false)) {
      g_mutex_unlock(&session_lock);
      buf_printf(out, "\"asleep\":true,\"locked\":%s", screen_locked() ? "true" : "false");
      return true;
    }
    wake_displays();
  }
  Monitor monitors[MAX_MONITORS];
  int count = monitors_of_streams(monitors);
  const Monitor *d = resolve_monitor(monitors, count, json_get(request, "display"));
  Stream *s = &streams[d - monitors];
  // Keep only the newest frame; a still screen produces none, so the last one stays valid. A new
  // stream gets up to 3 seconds for its first frame.
  for (int waited = 0;; waited++) {
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(s->sink), s->sample ? 0 : 100 * GST_MSECOND);
    if (sample) {
      if (s->sample) gst_sample_unref(s->sample);
      s->sample = sample;
      continue;
    }
    if (s->sample || waited >= 30) break;
  }
  if (!s->sample) {
    g_mutex_unlock(&session_lock);
    return fail("the screen stream has not produced a frame yet");
  }
  GstVideoInfo info;
  if (!gst_video_info_from_caps(&info, gst_sample_get_caps(s->sample))) {
    g_mutex_unlock(&session_lock);
    return fail("could not read the screen frame");
  }
  s->monitor.pixel_width = GST_VIDEO_INFO_WIDTH(&info);
  s->monitor.pixel_height = GST_VIDEO_INFO_HEIGHT(&info);
  Monitor shown = s->monitor;
  // The frame is encoded outside the lock, so input is not held up; the reference keeps it alive
  // even if the session is torn down meanwhile.
  GstSample *sample = gst_sample_ref(s->sample);
  g_mutex_unlock(&session_lock);
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstMapInfo map;
  bool ok;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    // BGRx is B, G, R, x in memory: as a little-endian word, red is at bit 16.
    ok = frame_reply(out, &shown, map.data, GST_VIDEO_INFO_PLANE_STRIDE(&info, 0), shown.pixel_width, shown.pixel_height,
                     16, 8, 0, request);
    gst_buffer_unmap(buffer, &map);
  } else {
    ok = fail("could not read the screen frame");
  }
  gst_sample_unref(sample);
  return ok;
}

// MARK: - Wayland: clipboard

static void on_owner_changed(GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                             const char *signal, GVariant *parameters, gpointer data) {
  GVariant *options = NULL;
  g_variant_get(parameters, "(o@a{sv})", NULL, &options);
  gboolean ours = FALSE;
  const char **types = NULL;
  g_variant_lookup(options, "session_is_owner", "b", &ours);
  g_variant_lookup(options, "mime_types", "^a&s", &types);
  g_mutex_lock(&clip_lock);
  clip_changes++;
  if (!ours) {
    clip_owner = false;
    g_strfreev(clip_offered);
    clip_offered = types ? g_strdupv((char **)types) : NULL;
  }
  g_mutex_unlock(&clip_lock);
  g_free(types);
  g_variant_unref(options);
}

static void on_transfer(GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                        const char *signal, GVariant *parameters, gpointer data) {
  const char *session;
  const char *mime;
  uint32_t serial;
  g_variant_get(parameters, "(&o&su)", &session, &mime, &serial);
  GUnixFDList *fds = NULL;
  GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_CLIPBOARD,
                                                                  "SelectionWrite", g_variant_new("(ou)", session, serial),
                                                                  G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                                                                  &fds, NULL, NULL);
  bool ok = false;
  if (reply) {
    gint32 index;
    g_variant_get(reply, "(h)", &index);
    int fd = g_unix_fd_list_get(fds, index, NULL);
    g_mutex_lock(&clip_lock);
    size_t written = 0;
    while (fd >= 0 && clip_text && written < clip_text_length) {
      ssize_t n = write(fd, clip_text + written, clip_text_length - written);
      if (n <= 0) break;
      written += (size_t)n;
    }
    ok = clip_text && written == clip_text_length;
    g_mutex_unlock(&clip_lock);
    if (fd >= 0) close(fd);
    g_variant_unref(reply);
    g_object_unref(fds);
  }
  GVariant *done = g_dbus_connection_call_sync(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_CLIPBOARD, "SelectionWriteDone",
                                               g_variant_new("(oub)", session, serial, ok), NULL, G_DBUS_CALL_FLAGS_NONE,
                                               5000, NULL, NULL);
  if (done) g_variant_unref(done);
}

static const char *text_mime(char **offered) {
  static const char *preferred[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT", NULL};
  for (int p = 0; preferred[p]; p++)
    for (int i = 0; offered && offered[i]; i++)
      if (!strcmp(offered[i], preferred[p])) return preferred[p];
  return NULL;
}

static bool read_clipboard(Buf *out) {
  g_mutex_lock(&session_lock);
  refresh_session();
  wait_for_session(3);
  char *handle = session_state == SESSION_READY ? g_strdup(session_handle) : NULL;
  g_mutex_unlock(&session_lock);
  g_mutex_lock(&clip_lock);
  buf_printf(out, "\"changeCount\":%lu", clip_changes);
  if (!handle || !clipboard_enabled) {
    g_mutex_unlock(&clip_lock);
    g_free(handle);
    return true;
  }
  if (clip_owner && clip_text) {
    buf_puts(out, ",\"text\":");
    buf_json_string(out, clip_text, clip_text_length);
    g_mutex_unlock(&clip_lock);
    g_free(handle);
    return true;
  }
  const char *mime = text_mime(clip_offered);
  g_mutex_unlock(&clip_lock);
  if (mime) {
    GUnixFDList *fds = NULL;
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_CLIPBOARD,
                                                                    "SelectionRead", g_variant_new("(os)", handle, mime),
                                                                    G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                                                                    &fds, NULL, NULL);
    if (reply) {
      gint32 index;
      g_variant_get(reply, "(h)", &index);
      int fd = g_unix_fd_list_get(fds, index, NULL);
      Buf text = {0};
      buf_reserve(&text, 0);
      char chunk[65536];
      double until = now_seconds() + 2;
      while (fd >= 0 && now_seconds() < until) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 200) <= 0) continue;
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        buf_put(&text, chunk, (size_t)n);
      }
      if (fd >= 0) close(fd);
      buf_puts(out, ",\"text\":");
      buf_json_string(out, text.data, text.len);
      buf_free(&text);
      g_variant_unref(reply);
      g_object_unref(fds);
    }
  }
  g_free(handle);
  return true;
}

/// Offers `text` on the clipboard; apps then ask for it through SelectionTransfer.
static bool clip_set(const char *handle, const char *text, size_t length) {
  g_mutex_lock(&clip_lock);
  bool enabled = clipboard_enabled;
  if (enabled) {
    g_free(clip_text);
    clip_text = g_strndup(text, length);
    clip_text_length = length;
  }
  g_mutex_unlock(&clip_lock);
  if (!enabled) return fail("this desktop did not allow clipboard access for screen sharing");
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  const char *types[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT", NULL};
  g_variant_builder_add(&options, "{sv}", "mime_types", g_variant_new_strv(types, -1));
  GVariant *reply = g_dbus_connection_call_sync(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_CLIPBOARD, "SetSelection",
                                                g_variant_new("(oa{sv})", handle, &options), NULL, G_DBUS_CALL_FLAGS_NONE,
                                                5000, NULL, NULL);
  if (!reply) return fail("could not set the host clipboard");
  g_variant_unref(reply);
  g_mutex_lock(&clip_lock);
  clip_owner = true;
  g_mutex_unlock(&clip_lock);
  return true;
}

// MARK: - Wayland: input

/// Sends one Notify* call and waits for the portal to accept it. The portal relays each call to the
/// compositor on its own, so calls sent without waiting can overtake each other: typed text came out
/// scrambled and clicks could land before the motion that should precede them.
static void notify(const char *method, GVariant *parameters) {
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_REMOTE, method, parameters, NULL,
                                                G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  if (reply) g_variant_unref(reply);
  // stderr ends up in the plugin's logs; one line per failed call is enough to see what broke.
  if (error) {
    fprintf(stderr, "portal %s failed: %s\n", method, error->message);
    g_error_free(error);
  }
}

static const int evdev_modifiers[4] = {EV_KEY_LEFTCTRL, EV_KEY_LEFTALT, EV_KEY_LEFTSHIFT, EV_KEY_LEFTMETA};
static const int modifier_bits[4] = {MOD_CTRL, MOD_ALT, MOD_SHIFT, MOD_SUPER};

static void key_code(int code, bool down) {
  notify("NotifyKeyboardKeycode", g_variant_new("(oa{sv}iu)", session_handle, NULL, code, down ? 1u : 0u));
}

static void key_sym(KeySym sym, bool down) {
  notify("NotifyKeyboardKeysym", g_variant_new("(oa{sv}iu)", session_handle, NULL, (gint32)sym, down ? 1u : 0u));
}

static void press_modifiers(int mask, bool down) {
  for (int k = 0; k < 4; k++) {
    int i = down ? k : 3 - k;
    if (mask & modifier_bits[i]) key_code(evdev_modifiers[i], down);
  }
}

/// Reads the stream's frame size in pixels from its negotiated caps. With display scaling it differs
/// from the logical size the portal reports (a 4K panel at 200% is 1920x1080 logical, 3840x2160 pixels).
static void update_pixel_size(Stream *s) {
  if (s->monitor.pixel_width || !s->sink) return;
  GstPad *pad = gst_element_get_static_pad(s->sink, "sink");
  GstCaps *caps = pad ? gst_pad_get_current_caps(pad) : NULL;
  GstVideoInfo info;
  if (caps && gst_video_info_from_caps(&info, caps)) {
    s->monitor.pixel_width = GST_VIDEO_INFO_WIDTH(&info);
    s->monitor.pixel_height = GST_VIDEO_INFO_HEIGHT(&info);
  }
  if (caps) gst_caps_unref(caps);
  if (pad) gst_object_unref(pad);
}

static void update_pixel_size(Stream *s);

/// Moves the pointer to (x, y) on the desktop.
///
/// GNOME with a scaled display (a 4K panel at 200%) disagrees with itself: the portal accepts
/// absolute positions only up to the stream's logical size, while Mutter reads them as physical
/// pixels, so the right and bottom of the screen are out of reach and everything lands at half
/// the distance. Relative motion is in logical units. So on Mutter with scaling, the pointer goes
/// to an anchor just inside the stream's corner (clear of the hot corner's pointer barrier at 0,0)
/// and moves the rest of the way relatively. Elsewhere one absolute motion does it.
static void pointer_to(int x, int y, const Monitor *monitors, int count) {
  const Monitor *m = monitor_containing(monitors, count, x, y);
  if (!m) m = &monitors[0];
  Stream *s = &streams[m - monitors];
  update_pixel_size(s);
  double sx = s->monitor.pixel_width && m->width ? (double)s->monitor.pixel_width / m->width : 1;
  double sy = s->monitor.pixel_height && m->height ? (double)s->monitor.pixel_height / m->height : 1;
  if (mutter && (sx != 1 || sy != 1)) {
    const double anchor = 10;  // physical pixels
    notify("NotifyPointerMotionAbsolute", g_variant_new("(oa{sv}udd)", session_handle, NULL, s->node, anchor, anchor));
    notify("NotifyPointerMotion",
           g_variant_new("(oa{sv}dd)", session_handle, NULL, (x - m->x) - anchor / sx, (y - m->y) - anchor / sy));
    return;
  }
  notify("NotifyPointerMotionAbsolute", g_variant_new("(oa{sv}udd)", session_handle, NULL, s->node, (double)(x - m->x), (double)(y - m->y)));
}

static int evdev_button(const Json *name) {
  unsigned int number = button_number(name);
  return number == 3 ? EV_BTN_RIGHT : number == 2 ? EV_BTN_MIDDLE : EV_BTN_LEFT;
}

static void button(int code, bool down) {
  notify("NotifyPointerButton", g_variant_new("(oa{sv}iu)", session_handle, NULL, code, down ? 1u : 0u));
}

/// Scroll deltas are pixels; the portal scrolls in wheel steps (positive is down or right).
static void scroll_steps(double *rest, double delta, uint32_t axis) {
  *rest += delta;
  int steps = (int)(*rest / PIXELS_PER_STEP);
  *rest -= steps * PIXELS_PER_STEP;
  if (steps > 100) steps = 100;
  if (steps < -100) steps = -100;
  if (steps) notify("NotifyPointerAxisDiscrete", g_variant_new("(oa{sv}ui)", session_handle, NULL, axis, steps));
}

/// Types text: line breaks and tabs as their keys, everything else as key symbols, which the
/// compositor maps onto the active layout.
static void type_text(const char *text, size_t length) {
  const char *p = text, *end = text + length;
  while (p < end) {
    uint32_t cp = next_codepoint(&p, end);
    if (cp == '\r' && p < end && *p == '\n') p++;
    if (cp == '\r' || cp == '\n') {
      key_code(EV_KEY_ENTER, true);
      key_code(EV_KEY_ENTER, false);
    } else if (cp == '\t') {
      key_code(EV_KEY_TAB, true);
      key_code(EV_KEY_TAB, false);
    } else {
      key_sym(keysym_for(cp), true);
      key_sym(keysym_for(cp), false);
    }
  }
}

static bool handle_input(const Json *request, Buf *out) {
  g_mutex_lock(&session_lock);
  refresh_session();
  wait_for_session(3);
  if (session_state != SESSION_READY || !(session_devices & (DEVICE_POINTER | DEVICE_KEYBOARD))) {
    g_mutex_unlock(&session_lock);
    return fail("%s", session_error[0] ? session_error : "Remote control has not been approved on the host yet.");
  }
  // Input is an explicit action, so it wakes a sleeping display before it lands.
  if (displays_asleep()) wake_displays();
  Monitor monitors[MAX_MONITORS];
  int count = monitors_of_streams(monitors);
  const Monitor *d = resolve_monitor(monitors, count, json_get(request, "display"));
  const Json *events = json_get(request, "events");
  size_t handled = events && events->type == J_ARR ? events->count : 0;
  bool ok = true;
  for (size_t i = 0; ok && i < handled; i++) {
    const Json *event = events->items[i];
    const char *type = json_str(json_get(event, "type"));
    int mods = modifier_mask(json_get(event, "modifiers"));
    int x, y;
    if (!type) {
      ok = fail("input event without a type");
    } else if (!strcmp(type, "move")) {
      desktop_point(event, d, monitors, count, &x, &y);
      pointer_to(x, y, monitors, count);
    } else if (!strcmp(type, "down") || !strcmp(type, "up")) {
      desktop_point(event, d, monitors, count, &x, &y);
      pointer_to(x, y, monitors, count);
      press_modifiers(mods, true);
      button(evdev_button(json_get(event, "button")), !strcmp(type, "down"));
      press_modifiers(mods, false);
    } else if (!strcmp(type, "click")) {
      desktop_point(event, d, monitors, count, &x, &y);
      pointer_to(x, y, monitors, count);
      int code = evdev_button(json_get(event, "button"));
      int clicks = (int)json_num(json_get(event, "count"), 1);
      clicks = clicks < 1 ? 1 : clicks > 3 ? 3 : clicks;
      press_modifiers(mods, true);
      for (int c = 0; c < clicks; c++) {
        button(code, true);
        button(code, false);
      }
      press_modifiers(mods, false);
    } else if (!strcmp(type, "scroll")) {
      if (json_get(event, "x")) {
        desktop_point(event, d, monitors, count, &x, &y);
        pointer_to(x, y, monitors, count);
      }
      scroll_steps(&scroll_rest_y, fmin(10000, fmax(-10000, json_num(json_get(event, "dy"), 0))), 0);
      scroll_steps(&scroll_rest_x, fmin(10000, fmax(-10000, json_num(json_get(event, "dx"), 0))), 1);
    } else if (!strcmp(type, "key")) {
      // A character becomes a key symbol on the host's layout; a key code is a physical position.
      const char *chars = json_str(json_get(event, "char"));
      const Json *down = json_get(event, "down");
      int code = -1;
      KeySym sym = 0;
      if (chars && *chars) {
        const char *p = chars;
        uint32_t cp = next_codepoint(&p, chars + strlen(chars));
        if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
        sym = keysym_for(cp);
      } else {
        double key = json_num(json_get(event, "keyCode"), -1);
        if (key >= 0 && key < 128 && mac_to_x[(int)key]) code = mac_to_x[(int)key] - 8;
      }
      if (code < 0 && !sym) {
        ok = fail("no key for %s", chars ? chars : "this key code");
        break;
      }
      press_modifiers(mods, true);
      bool presses[2] = {true, false};
      int n = down && down->type == J_BOOL ? 1 : 2;
      if (n == 1) presses[0] = down->num != 0;
      for (int k = 0; k < n; k++) {
        if (sym) key_sym(sym, presses[k]);
        else key_code(code, presses[k]);
      }
      press_modifiers(mods, false);
    } else if (!strcmp(type, "text")) {
      const Json *text = json_get(event, "text");
      if (text && text->type == J_STR) type_text(text->str, text->len);
    } else if (!strcmp(type, "paste")) {
      // Puts the text on the host clipboard and presses Ctrl+V (Ctrl+Shift+V with shift, for terminals).
      const Json *text = json_get(event, "text");
      if (!clip_set(session_handle, text && text->type == J_STR ? text->str : "", text && text->type == J_STR ? text->len : 0)) {
        ok = false;
        break;
      }
      usleep(30000);
      int paste_mods = MOD_CTRL | (json_bool(json_get(event, "shift"), false) ? MOD_SHIFT : 0);
      press_modifiers(paste_mods, true);
      key_sym('v', true);
      key_sym('v', false);
      press_modifiers(paste_mods, false);
    } else {
      ok = fail("unknown input event: %s", type);
    }
  }
  g_mutex_unlock(&session_lock);
  if (!ok) return false;
  buf_printf(out, "\"handled\":%zu", handled);
  if (has_last_pointer) {
    const Monitor *landed = monitor_containing(monitors, count, last_pointer_x, last_pointer_y);
    if (landed) buf_printf(out, ",\"pointerDisplay\":%lu", landed->id);
  }
  return true;
}

// MARK: - Wayland: status

static void status(Buf *out) {
  g_mutex_lock(&session_lock);
  refresh_session();
  bool ready = session_state == SESSION_READY;
  buf_printf(out, "\"backend\":\"wayland\",\"screenRecording\":%s,\"accessibility\":%s,\"awaitingApproval\":%s,\"locked\":%s,\"displays\":[",
             ready ? "true" : "false", ready && (session_devices & (DEVICE_POINTER | DEVICE_KEYBOARD)) ? "true" : "false",
             session_state == SESSION_STARTING ? "true" : "false", screen_locked() ? "true" : "false");
  for (int i = 0; ready && i < stream_count; i++) {
    if (i) buf_puts(out, ",");
    update_pixel_size(&streams[i]);
    monitor_json(out, &streams[i].monitor);
  }
  buf_puts(out, "]");
  if (session_error[0]) {
    buf_puts(out, ",\"permissionError\":");
    buf_json_string(out, session_error, strlen(session_error));
  }
  g_mutex_unlock(&session_lock);
}

/// Shows the desktop's screen sharing dialog on the host, unless a session is running or starting.
/// A session approved without remote interaction is view-only, and its saved token would keep it so;
/// asking again then forgets the token, so the dialog appears and input can be allowed.
static bool request_permissions(Buf *out) {
  g_mutex_lock(&session_lock);
  refresh_session();
  if (session_state == SESSION_READY && !(session_devices & (DEVICE_POINTER | DEVICE_KEYBOARD))) {
    teardown_session();
    unlink(token_file);
  }
  begin_session();
  g_mutex_unlock(&session_lock);
  status(out);
  return true;
}
#endif

// MARK: - Main loop

#ifndef PASEO_WAYLAND
static void connect_display(void) {
  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    open_error = "Could not open the X display. Run Paseo from the desktop session, so DISPLAY and XAUTHORITY are set.";
    return;
  }
  XSetErrorHandler(on_x_error);
  screen_number = DefaultScreen(dpy);
  root = RootWindow(dpy, screen_number);
  int event_base, error_base, major, minor;
  has_xtest = XTestQueryExtension(dpy, &event_base, &error_base, &major, &minor);
  has_shm = XShmQueryExtension(dpy);
  has_dpms = DPMSQueryExtension(dpy, &event_base, &error_base) && DPMSCapable(dpy);
  has_fixes = XFixesQueryExtension(dpy, &event_base, &error_base);
  has_monitors = XRRQueryExtension(dpy, &event_base, &error_base) && XRRQueryVersion(dpy, &major, &minor) &&
                 (major > 1 || (major == 1 && minor >= 5));

  KeySym modifiers[4] = {XK_Control_L, XK_Alt_L, XK_Shift_L, XK_Super_L};
  KeyCode fallbacks[4] = {37, 64, 50, 133};
  for (int i = 0; i < 4; i++) {
    modifier_keys[i] = XKeysymToKeycode(dpy, modifiers[i]);
    if (!modifier_keys[i]) modifier_keys[i] = fallbacks[i];
  }
  // The highest key code with nothing mapped to it is free to borrow for typing.
  int min_code, max_code, per_code;
  XDisplayKeycodes(dpy, &min_code, &max_code);
  KeySym *map = XGetKeyboardMapping(dpy, (KeyCode)min_code, max_code - min_code + 1, &per_code);
  if (map) {
    for (int code = max_code; code >= min_code && !scratch_keycode; code--) {
      bool empty = true;
      for (int k = 0; k < per_code; k++)
        if (map[(code - min_code) * per_code + k] != NoSymbol) empty = false;
      if (empty) scratch_keycode = (KeyCode)code;
    }
    XFree(map);
  }
}
#endif

static void handle(char *line) {
  Parser parser = {line};
  Json *request = parse_value(&parser, 0);
  Buf out = {0};
  if (!request || request->type != J_OBJ) {
    buf_puts(&out, "{\"ok\":false,\"error\":\"invalid JSON request\"}");
  } else {
    const Json *id = json_get(request, "id");
    buf_puts(&out, "{\"id\":");
    if (id && id->type == J_NUM) buf_printf(&out, "%.0f", id->num);
    else buf_puts(&out, "null");
    Buf result = {0};
    buf_reserve(&result, 0);
    const char *command = json_str(json_get(request, "cmd"));
    bool ok;
    error_message[0] = 0;
    if (open_error) ok = fail("%s", open_error);
    else if (!command) ok = fail("unknown command: nil");
    else if (!strcmp(command, "status")) {
      status(&result);
      ok = true;
    } else if (!strcmp(command, "request")) ok = request_permissions(&result);
    else if (!strcmp(command, "frame")) ok = capture_frame(request, &result);
    else if (!strcmp(command, "input")) ok = handle_input(request, &result);
    else if (!strcmp(command, "clipboard")) ok = read_clipboard(&result);
    else ok = fail("unknown command: %s", command);
    if (ok) {
      buf_puts(&out, ",\"ok\":true");
      if (result.len) {
        buf_puts(&out, ",");
        buf_put(&out, result.data, result.len);
      }
    } else {
      buf_puts(&out, ",\"ok\":false,\"error\":");
      buf_json_string(&out, error_message, strlen(error_message));
    }
    buf_puts(&out, "}");
    buf_free(&result);
  }
  json_free(request);
  buf_put(&out, "\n", 1);
  static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&output_lock);
  fwrite(out.data, 1, out.len, stdout);
  fflush(stdout);
  pthread_mutex_unlock(&output_lock);
  buf_free(&out);
}

#ifndef PASEO_WAYLAND
int main(void) {
  signal(SIGPIPE, SIG_IGN);
  XInitThreads();
  connect_display();
  char *line = NULL;
  size_t capacity = 0;
  ssize_t length;
  while ((length = getline(&line, &capacity, stdin)) >= 0) {
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = 0;
    if (length == 0) continue;
    handle(line);
  }
  free(line);
  if (dpy) {
    free_shm_image();
    XCloseDisplay(dpy);
  }
  return 0;
}
#endif

#ifdef PASEO_WAYLAND
// One helper serves both frames and input on Wayland, because each process would need a portal
// session (and the host's approval) of its own. Frames are answered on their own thread so that
// input never waits behind a capture; replies carry ids, so they may finish out of order.
static GAsyncQueue *frame_requests;

static gpointer frame_thread(gpointer unused) {
  for (;;) {
    char *line = g_async_queue_pop(frame_requests);
    if (!*line) {
      g_free(line);
      return NULL;
    }
    handle(line);
    g_free(line);
  }
}

static bool is_frame_request(const char *line) {
  Parser parser = {line};
  Json *request = parse_value(&parser, 0);
  const char *command = request && request->type == J_OBJ ? json_str(json_get(request, "cmd")) : NULL;
  bool frame = command && !strcmp(command, "frame");
  json_free(request);
  return frame;
}

static gpointer stdin_thread(gpointer unused) {
  frame_requests = g_async_queue_new();
  GThread *frames = g_thread_new("frames", frame_thread, NULL);
  char *line = NULL;
  size_t capacity = 0;
  ssize_t length;
  while ((length = getline(&line, &capacity, stdin)) >= 0) {
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = 0;
    if (length == 0) continue;
    if (is_frame_request(line)) g_async_queue_push(frame_requests, g_strdup(line));
    else handle(line);
  }
  free(line);
  g_async_queue_push(frame_requests, g_strdup(""));  // an empty line stops the frame thread
  g_thread_join(frames);
  // A start still in progress is left to the desktop, which closes it with this process's bus connection.
  g_mutex_lock(&session_lock);
  if (session_state == SESSION_READY) teardown_session();
  g_mutex_unlock(&session_lock);
  g_main_loop_quit(loop);
  return NULL;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  gst_init(NULL, NULL);
  GError *error = NULL;
  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus) {
    static char message[512];
    snprintf(message, sizeof message, "Could not reach the desktop session's D-Bus (%s). Start Paseo from the desktop session.",
             error ? error->message : "unknown error");
    open_error = message;
  }
  const char *config = g_get_user_config_dir();
  snprintf(token_file, sizeof token_file, "%s/paseo-screen/wayland-restore-token", config);
  loop = g_main_loop_new(NULL, FALSE);
  if (bus) {
    // Clipboard signals are for any session of ours; the handlers check what they need.
    g_dbus_connection_signal_subscribe(bus, PORTAL_BUS, PORTAL_CLIPBOARD, "SelectionOwnerChanged", PORTAL_PATH, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_owner_changed, NULL, NULL);
    g_dbus_connection_signal_subscribe(bus, PORTAL_BUS, PORTAL_CLIPBOARD, "SelectionTransfer", PORTAL_PATH, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_transfer, NULL, NULL);
  }
  g_thread_unref(g_thread_new("stdin", stdin_thread, NULL));
  g_main_loop_run(loop);
  return 0;
}
#endif
