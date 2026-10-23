// paseo-screen-helper for Linux: captures the screen and injects input for the paseo-screen plugin
// on an X11 session. It speaks the same protocol as helper/main.swift.
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
// Build: cc -O2 -std=gnu11 -o paseo-screen-helper linux.c -lX11 -lXext -lXtst -lXrandr -lXfixes -ljpeg -lpthread -lm

#define _GNU_SOURCE
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
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>
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

static char error_message[1024];

static bool fail(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(error_message, sizeof error_message, format, args);
  va_end(args);
  return false;
}

// MARK: - X connection

static Display *dpy;
static Window root;
static int screen_number;
static bool has_xtest, has_shm, has_dpms, has_monitors, has_fixes;
static const char *open_error;

/// X errors would otherwise end the process. They are recorded for the connection that caused them.
static Display *clip_dpy;
static volatile bool main_x_error;

static int on_x_error(Display *display, XErrorEvent *event) {
  (void)event;
  if (display == dpy) main_x_error = true;
  return 0;
}

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// MARK: - Displays

typedef struct {
  unsigned long id;
  int x, y, width, height;
  bool main, builtin;
  char name[96];
} Monitor;

#define MAX_MONITORS 16

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

static void monitor_json(Buf *b, const Monitor *d) {
  buf_printf(b, "{\"id\":%lu,\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"pixelWidth\":%d,\"pixelHeight\":%d,\"main\":%s,\"builtin\":%s,\"name\":",
             d->id, d->x, d->y, d->width, d->height, d->width, d->height, d->main ? "true" : "false", d->builtin ? "true" : "false");
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

/// True while the session's lock screen is showing. Checked at most every two seconds.
static bool screen_locked(void) {
  static double checked_at = -100;
  static bool locked = false;
  double now = now_seconds();
  if (now - checked_at < 2) return locked;
  checked_at = now;
  int active = screensaver_active("org.gnome.ScreenSaver", "/org/gnome/ScreenSaver");
  if (active < 0) active = screensaver_active("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver");
  locked = active == 1;
  return locked;
}

// MARK: - Capture

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

static int mask_shift(unsigned long mask) {
  int shift = 0;
  while (mask && !(mask & 1)) {
    mask >>= 1;
    shift++;
  }
  return shift;
}

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

/// Scales the image down to width x height RGB by averaging each source block.
static uint8_t *scale_to_rgb(const XImage *image, int width, int height) {
  uint8_t *rgb = malloc((size_t)width * (size_t)height * 3);
  int *columns = malloc(((size_t)width + 1) * sizeof *columns);
  if (!rgb || !columns) abort();
  for (int i = 0; i <= width; i++) columns[i] = (int)((long long)i * image->width / width);
  int rs = mask_shift(image->red_mask), gs = mask_shift(image->green_mask), bs = mask_shift(image->blue_mask);
  for (int oy = 0; oy < height; oy++) {
    int y0 = (int)((long long)oy * image->height / height);
    int y1 = (int)((long long)(oy + 1) * image->height / height);
    if (y1 <= y0) y1 = y0 + 1;
    uint8_t *out = rgb + (size_t)oy * (size_t)width * 3;
    for (int ox = 0; ox < width; ox++) {
      int x0 = columns[ox], x1 = columns[ox + 1];
      if (x1 <= x0) x1 = x0 + 1;
      uint32_t r = 0, g = 0, b = 0, n = 0;
      for (int y = y0; y < y1; y++) {
        const uint32_t *row = (const uint32_t *)(image->data + (size_t)y * (size_t)image->bytes_per_line);
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
  double max_width = json_num(json_get(request, "maxWidth"), 1600);
  if (max_width < 320) max_width = 320;
  double quality = json_num(json_get(request, "quality"), 0.6);
  quality = quality < 0.1 ? 0.1 : quality > 1 ? 1 : quality;

  XImage *image = grab(d->x, d->y, d->width, d->height);
  if (!image) return fail("could not read the screen of display %lu", d->id);
  if (image->bits_per_pixel != 32 || image->byte_order != LSBFirst) {
    int bits = image->bits_per_pixel;
    if (image != shm_image) XDestroyImage(image);
    return fail("unsupported screen pixel format (%d bits per pixel)", bits);
  }
  draw_cursor(image, d);
  double scale = max_width / d->width < 1 ? max_width / d->width : 1;
  int width = (int)(d->width * scale), height = (int)(d->height * scale);
  if (width < 1) width = 1;
  if (height < 1) height = 1;
  uint8_t *rgb = scale_to_rgb(image, width, height);
  if (image != shm_image) XDestroyImage(image);

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

/// US ANSI positions (as macOS key codes) for letters a layout cannot produce.
static const signed char ansi_letters[26] = {
  0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6,
};

enum { MOD_CTRL = 1, MOD_ALT = 2, MOD_SHIFT = 4, MOD_SUPER = 8 };

// Left-side modifier keys in press order, like the macOS helper.
static KeyCode modifier_keys[4];
static const int modifier_order[4] = {MOD_CTRL, MOD_ALT, MOD_SHIFT, MOD_SUPER};
static KeyCode scratch_keycode;  // an unused key code, remapped to type characters the layout lacks
static bool scratch_mapped;
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

static KeySym keysym_for(uint32_t cp) {
  if (cp == '\n' || cp == '\r') return XK_Return;
  if (cp == '\t') return XK_Tab;
  if (cp == '\b') return XK_BackSpace;
  if ((cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0xFF)) return cp;
  return 0x01000000 | cp;
}

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
    KeySym none = NoSymbol;
    XChangeKeyboardMapping(dpy, scratch_keycode, 1, &none, 1);
    scratch_mapped = false;
  }
}

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

/// Scroll deltas are pixels; X scrolls in wheel steps. Leftover pixels carry over to the next event.
#define PIXELS_PER_STEP 50.0

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

// MARK: - Main loop

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
    if (!dpy) ok = fail("%s", open_error);
    else if (!command) ok = fail("unknown command: nil");
    else if (!strcmp(command, "status") || !strcmp(command, "request")) {
      status(&result);
      ok = true;
    } else if (!strcmp(command, "frame")) ok = capture_frame(request, &result);
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
  fwrite(out.data, 1, out.len, stdout);
  fflush(stdout);
  buf_free(&out);
}

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
