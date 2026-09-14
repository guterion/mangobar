#define _GNU_SOURCE
#include "style.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void apply_rule(Style *dst, const StyleRule *r);

static void skip_ws_comments(const char **p) {
  for (;;) {
    const char *s = *p;
    while (*s && isspace((unsigned char)*s))
      s++;
    if (s[0] == '/' && s[1] == '*') {
      s += 2;
      while (*s && !(s[0] == '*' && s[1] == '/'))
        s++;
      if (*s)
        s += 2;
      *p = s;
    } else {
      *p = s;
      return;
    }
  }
}

// Parse #RGB / #RGBA / #RRGGBB / #AARRGGBB -> ARGB (uint32)
static int parse_hex_color(const char *s, uint32_t *out) {
  if (*s == '#')
    s++;
  size_t n = strlen(s);
  if (n != 3 && n != 4 && n != 6 && n != 8)
    return 0;
  for (size_t i = 0; i < n; i++)
    if (!isxdigit((unsigned char)s[i]))
      return 0;
  char buf[16];
  int b = 0;
  if (n == 3 || n == 4) {
    for (size_t i = 0; i < n; i++) {
      buf[b++] = s[i];
      buf[b++] = s[i];
    }
    buf[b] = '\0';
  } else {
    strcpy(buf, s);
    b = (int)n;
  }
  unsigned long v = strtoul(buf, NULL, 16);
  if (b == 6)
    v = (v << 8) | 0xFFu; // fill alpha when absent
  *out = (uint32_t)v;
  return 1;
}

static int parse_int(const char *s) {
  while (*s && !isdigit((unsigned char)*s))
    s++;
  if (!isdigit((unsigned char)*s))
    return 0;
  int sign = 1;
  const char *q = s;
  while (q > s && (q[-1] == '-' || q[-1] == '+')) {
    if (q[-1] == '-')
      sign = -sign;
    q--;
  }
  return sign * atoi(s);
}

static const char *find_var(const StyleSheet *ss, const char *name) {
  for (int i = 0; i < ss->var_count; i++)
    if (strcmp(ss->var_names[i], name) == 0)
      return ss->var_values[i];
  return NULL;
}

// Resolve a color value to hex:
// supports #hex, @var, linear-gradient(...) (first color), none (transparent)
static int resolve_color_value(const StyleSheet *ss, const char *val,
                               char *out, size_t outsz) {
  const char *p = val;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (strncmp(p, "none", 4) == 0) {
    snprintf(out, outsz, "#00000000");
    return 1;
  }
  if (*p == '#') {
    snprintf(out, outsz, "%s", p);
    return 1;
  }
  if (*p == '@') {
    p++;
    char name[64];
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n]) && p[n] != ';' &&
           p[n] != ')' && n + 1 < sizeof(name))
      {
        name[n] = p[n];
        n++;
      }
    name[n] = '\0';
    const char *v = find_var(ss, name);
    if (v)
      snprintf(out, outsz, "%s", v);
    return v ? 1 : 0;
  }
  // Find first #hex or @var inside parentheses
  while (*p) {
    if (*p == '#') {
      const char *q = p;
      while (*q && !isspace((unsigned char)*q) && *q != ')' && *q != ',')
        q++;
      size_t n = (size_t)(q - p);
      if (n < outsz) {
        memcpy(out, p, n);
        out[n] = '\0';
        return 1;
      }
      return 0;
    }
    if (*p == '@') {
      p++;
      char name[64];
      size_t n = 0;
      while (p[n] && !isspace((unsigned char)p[n]) && p[n] != ')' &&
             p[n] != ',' && n + 1 < sizeof(name))
        {
          name[n] = p[n];
          n++;
        }
      name[n] = '\0';
      const char *v = find_var(ss, name);
      if (v) {
        snprintf(out, outsz, "%s", v);
        return 1;
      }
      return 0;
    }
    p++;
  }
  return 0;
}

// Mix two ARGB colors with ratio t (0..1), interpolating each channel.
static uint32_t mix_colors(uint32_t a, uint32_t b, double t) {
  if (t < 0.0)
    t = 0.0;
  if (t > 1.0)
    t = 1.0;
  uint32_t out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    double ca = (a >> sh) & 0xFF;
    double cb = (b >> sh) & 0xFF;
    out |= (uint32_t)(ca + (cb - ca) * t + 0.5) << sh;
  }
  return out;
}

// Resolve a single color token: #hex, @var, or mix(color, color, t).
static int parse_color_token(const StyleSheet *ss, const char *tok,
                             uint32_t *out, int depth) {
  if (depth > 8)
    return 0;
  while (*tok && isspace((unsigned char)*tok))
    tok++;
  if (strncmp(tok, "mix(", 4) == 0) {
    const char *p = tok + 4;
    // Split the arguments on top-level commas (track paren depth).
    const char *args[3] = {NULL, NULL, NULL};
    int ai = 0;
    int depth2 = 0;
    const char *start = p;
    for (; *p && ai < 3; p++) {
      if (*p == '(')
        depth2++;
      else if (*p == ')') {
        if (depth2 == 0)
          break;
        depth2--;
      } else if (*p == ',' && depth2 == 0) {
        if (ai < 3)
          args[ai++] = start;
        start = p + 1;
      }
    }
    if (ai == 2) {
      args[2] = start;
      char tbuf[32] = {0};
      uint32_t c1, c2;
      const char *q = args[2];
      size_t n = 0;
      while (*q && *q != ')' && n + 1 < sizeof(tbuf))
        tbuf[n++] = *q++;
      if (parse_color_token(ss, args[0], &c1, depth + 1) &&
          parse_color_token(ss, args[1], &c2, depth + 1)) {
        *out = mix_colors(c1, c2, atof(tbuf));
        return 1;
      }
    }
    return 0;
  }
  if (*tok == '@') {
    char name[64];
    size_t n = 0;
    tok++;
    while (tok[n] && !isspace((unsigned char)tok[n]) && tok[n] != ',' &&
           tok[n] != ')' && n + 1 < sizeof(name))
      {
        name[n] = tok[n];
        n++;
      }
    name[n] = '\0';
    const char *v = find_var(ss, name);
    return v ? parse_color_token(ss, v, out, depth + 1) : 0;
  }
  if (*tok == '#') {
    // Truncate at delimiters so a trailing ')' from a gradient is ignored.
    char buf[16];
    size_t n = 0;
    while (tok[n] && n + 1 < sizeof(buf) &&
           !isspace((unsigned char)tok[n]) && tok[n] != ')' && tok[n] != ',')
      {
        buf[n] = tok[n];
        n++;
      }
    buf[n] = '\0';
    return parse_hex_color(buf, out);
  }
  if (strncmp(tok, "transparent", 11) == 0 ||
      strncmp(tok, "none", 4) == 0) {
    *out = 0;
    return 1;
  }
  return 0;
}

// Parse "linear-gradient(to top, c1, c2)" (or to bottom/left/right; plain
// two-color gradients; stops may use @vars and mix()). Fills start/end ARGB
// and direction. Returns 1 on success.
static int parse_linear_gradient(const StyleSheet *ss, const char *val,
                                 uint32_t *c1, uint32_t *c2, int *dir) {
  const char *p = val;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (strncasecmp(p, "linear-gradient", 15) != 0)
    return 0;
  p += 15;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p != '(')
    return 0;
  p++;

  // Everything until the matching ')' is the argument list.
  int depth = 1;
  const char *start = p;
  while (*p && depth > 0) {
    if (*p == '(')
      depth++;
    else if (*p == ')')
      depth--;
    if (depth > 0)
      p++;
  }
  size_t len = (size_t)(p - start);
  if (len == 0)
    return 0;
  char argbuf[512];
  if (len >= sizeof(argbuf))
    len = sizeof(argbuf) - 1;
  memcpy(argbuf, start, len);
  argbuf[len] = '\0';

  *dir = 0; // default: to bottom
  char *q = argbuf;
  while (*q && isspace((unsigned char)*q))
    q++;
  if (strncmp(q, "to ", 3) == 0) {
    q += 3;
    while (*q && isspace((unsigned char)*q))
      q++;
    if (strncmp(q, "top", 3) == 0) {
      *dir = 1;
      q += 3;
    } else if (strncmp(q, "bottom", 6) == 0) {
      *dir = 0;
      q += 6;
    } else if (strncmp(q, "left", 4) == 0) {
      *dir = 2;
      q += 4;
    } else if (strncmp(q, "right", 5) == 0) {
      *dir = 3;
      q += 5;
    }
    while (*q && *q != ',')
      q++;
    if (*q == ',')
      q++;
  }

  // Split the two color stops on top-level commas.
  const char *stops[2] = {q, NULL};
  int si = 1;
  int d2 = 0;
  char *s = q;
  for (; *s && si < 2; s++) {
    if (*s == '(')
      d2++;
    else if (*s == ')') {
      if (d2 > 0)
        d2--;
    } else if (*s == ',' && d2 == 0) {
      stops[si++] = s + 1;
      *s = '\0';
    }
  }
  if (si < 2)
    return 0;
  return parse_color_token(ss, stops[0], c1, 0) &&
         parse_color_token(ss, stops[1], c2, 0);
}

// Parse 1-2 ints: one value -> all sides; two -> vertical, horizontal
static void parse_int_pair(const char *s, int *a, int *b) {
  *a = parse_int(s);
  const char *p = s;
  while (*p && !isdigit((unsigned char)*p))
    p++;
  if (!*p)
    return;
  while (*p && (isdigit((unsigned char)*p) || *p == '-' || *p == '+'))
    p++;
  while (*p && !isdigit((unsigned char)*p))
    p++;
  if (*p)
    *b = parse_int(p);
  else
    *b = *a;
}

// Copy the next item of a list into out and advance *p past it. The items
// are separated by sep, or by white space when sep is ' ', and a
// parenthesised group counts as part of one item. Returns false at the end.
static bool next_token(const char **p, char sep, char *out, size_t outsz) {
  const char *s = *p;
  while (*s && (isspace((unsigned char)*s) || *s == sep))
    s++;
  if (!*s) {
    *p = s;
    return false;
  }
  const char *start = s;
  int depth = 0;
  while (*s && (depth > 0 || (sep == ' ' ? !isspace((unsigned char)*s)
                                         : *s != sep))) {
    if (*s == '(')
      depth++;
    else if (*s == ')' && depth > 0)
      depth--;
    s++;
  }
  size_t n = (size_t)(s - start);
  while (n > 0 && isspace((unsigned char)start[n - 1]))
    n--;
  if (n >= outsz)
    n = outsz - 1;
  memcpy(out, start, n);
  out[n] = '\0';
  *p = s;
  return true;
}

// Read a length in pixels: "0", "3px" or "-3px". Returns false for another
// unit, such as a percentage.
static bool parse_px(const char *tok, int *out) {
  const char *d = tok[0] == '-' ? tok + 1 : tok;
  if (!isdigit((unsigned char)*d))
    return false;
  const char *u = d;
  while (isdigit((unsigned char)*u))
    u++;
  if (*u && strcmp(u, "px") != 0)
    return false;
  *out = tok[0] == '-' ? -parse_int(d) : parse_int(d);
  return true;
}

// Parse "repeating-linear-gradient(to <side>, <stop>, ...)". A stop is a
// color with zero, one or two lengths in pixels. The first stop starts at
// 0, a stop without a length takes the position before it, and a position
// never goes back. The last stop must name the period.
static bool parse_repeating_gradient(StyleSheet *ss, const char *val,
                                     BackgroundLayer *out) {
  static const char name[] = "repeating-linear-gradient(";
  if (strncmp(val, name, sizeof(name) - 1) != 0)
    return false;
  const char *open = val + sizeof(name) - 1;
  const char *close = strrchr(open, ')');
  if (!close)
    return false;
  char args[512];
  size_t n = (size_t)(close - open);
  if (n >= sizeof(args))
    return false;
  memcpy(args, open, n);
  args[n] = '\0';

  memset(out, 0, sizeof(*out));
  const char *p = args;
  char item[128];
  bool first = true;
  bool period = false;
  int last = 0;
  while (next_token(&p, ',', item, sizeof(item))) {
    if (first && strncmp(item, "to ", 3) == 0) {
      const char *side = item + 3;
      while (isspace((unsigned char)*side))
        side++;
      if (strcmp(side, "bottom") == 0)
        out->dir = 0;
      else if (strcmp(side, "top") == 0)
        out->dir = 1;
      else if (strcmp(side, "left") == 0)
        out->dir = 2;
      else if (strcmp(side, "right") == 0)
        out->dir = 3;
      else
        return false;
      first = false;
      continue;
    }
    first = false;
    uint32_t color = 0;
    bool have_color = false;
    int pos[2];
    int npos = 0;
    const char *q = item;
    char tok[64];
    while (next_token(&q, ' ', tok, sizeof(tok))) {
      int v;
      if (npos < 2 && parse_px(tok, &v))
        pos[npos++] = v;
      else if (!have_color && parse_color_token(ss, tok, &color, 0))
        have_color = true;
      else
        return false;
    }
    if (!have_color)
      return false;
    // A stop with two lengths is two stops of the same color.
    for (int i = 0; i < (npos == 2 ? 2 : 1); i++) {
      if (out->stop_count >= MANGOBAR_MAX_GRADIENT_STOPS)
        return false;
      int at = npos > i ? pos[i] : last;
      if (at < last)
        at = last;
      out->stops[out->stop_count++] = (GradientStop){color, at};
      last = at;
    }
    period = npos > 0;
  }
  return period && out->stop_count >= 2 && last > 0;
}

// Parse one shadow layer: two to four lengths, a color and, for a box, the
// inset keyword, in any order. text-shadow takes three lengths at the most.
// A parenthesised group such as mix(a, b, t) counts as one token.
static bool parse_shadow(StyleSheet *ss, const char *item, bool box,
                         Shadow *out) {
  int lengths[5] = {0, 0, 0, 0, 0};
  int nlen = 0;
  bool have_color = false;
  const char *p = item;
  char tok[128];
  while (next_token(&p, ' ', tok, sizeof(tok))) {

    bool number = isdigit((unsigned char)tok[0]) ||
                  (tok[0] == '-' && isdigit((unsigned char)tok[1]));
    char color_buf[64];
    uint32_t c;
    if (number) {
      // parse_int reads the digits alone, so the sign is taken here.
      if (nlen < 5)
        lengths[nlen++] = tok[0] == '-' ? -parse_int(tok + 1) : parse_int(tok);
    } else if (strcmp(tok, "inset") == 0) {
      out->inset = true;
    } else if (resolve_color_value(ss, tok, color_buf, sizeof(color_buf)) &&
               parse_hex_color(color_buf, &c)) {
      out->color = c;
      have_color = true;
    }
  }
  if (!have_color || nlen < 2 || nlen > (box ? 4 : 3) || (out->inset && !box))
    return false;
  out->x = lengths[0];
  out->y = lengths[1];
  out->blur = nlen > 2 && lengths[2] > 0 ? lengths[2] : 0;
  out->spread = lengths[3];
  return true;
}

// Parse a list of shadow layers separated by commas outside parentheses.
// The whole list replaces the list that a wider rule set.
static void parse_shadow_list(StyleSheet *ss, const char *val, bool box,
                              Shadow *out, int *count, bool *set) {
  *count = 0;
  *set = true;
  const char *p = val;
  while (*p && *count < MANGOBAR_MAX_SHADOWS) {
    const char *start = p;
    int depth = 0;
    while (*p && (depth > 0 || *p != ',')) {
      if (*p == '(')
        depth++;
      else if (*p == ')' && depth > 0)
        depth--;
      p++;
    }
    char item[256];
    size_t n = (size_t)(p - start);
    if (n >= sizeof(item))
      n = sizeof(item) - 1;
    memcpy(item, start, n);
    item[n] = '\0';
    Shadow sh = {0};
    if (strstr(item, "none") == NULL && parse_shadow(ss, item, box, &sh))
      out[(*count)++] = sh;
    if (*p == ',')
      p++;
  }
}

static void apply_decl(StyleSheet *ss, Style *st, const char *prop,
                       const char *val) {
  uint32_t c;
  char color_buf[64];
  if (strcmp(prop, "color") == 0 &&
      resolve_color_value(ss, val, color_buf, sizeof(color_buf)) &&
      parse_hex_color(color_buf, &c)) {
    st->color = c;
    st->color_set = true;
  } else if (strcmp(prop, "background") == 0 ||
             strcmp(prop, "background-color") == 0) {
    uint32_t c1, c2;
    int dir;
    if (parse_linear_gradient(ss, val, &c1, &c2, &dir)) {
      st->background = c1;
      st->gradient_end = c2;
      st->gradient_dir = dir;
      st->bg_gradient = true;
      st->background_set = true;
    } else if (resolve_color_value(ss, val, color_buf, sizeof(color_buf)) &&
               parse_hex_color(color_buf, &c)) {
      st->background = c;
      st->bg_gradient = false;
      st->background_set = true;
    }
  } else if (strcmp(prop, "border-color") == 0 &&
             resolve_color_value(ss, val, color_buf, sizeof(color_buf)) &&
             parse_hex_color(color_buf, &c)) {
    st->border_color = c;
    st->border_color_set = true;
  } else if (strcmp(prop, "border-width") == 0) {
    st->border_width = parse_int(val);
    st->border_width_set = true;
  } else if (strcmp(prop, "border") == 0) {
    // Width, style and color may come in any order; the style keyword is
    // read and ignored, because the border is always solid.
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", val);
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok;
         tok = strtok_r(NULL, " \t", &save)) {
      if (strcmp(tok, "none") == 0) {
        st->border_width = 0;
        st->border_width_set = true;
      } else if (isdigit((unsigned char)tok[0])) {
        st->border_width = parse_int(tok);
        st->border_width_set = true;
      } else if (resolve_color_value(ss, tok, color_buf, sizeof(color_buf)) &&
                 parse_hex_color(color_buf, &c)) {
        st->border_color = c;
        st->border_color_set = true;
      }
    }
  } else if (strcmp(prop, "text-shadow") == 0) {
    parse_shadow_list(ss, val, false, st->text_shadows, &st->text_shadow_count,
                      &st->text_shadow_set);
  } else if (strcmp(prop, "background-image") == 0) {
    // A list of layers separated by commas outside parentheses. The whole
    // list replaces the list that a wider rule set, and "none" clears it.
    st->bg_layer_count = 0;
    st->bg_layers_set = true;
    const char *p = val;
    char item[512];
    while (next_token(&p, ',', item, sizeof(item)) &&
           st->bg_layer_count < MANGOBAR_MAX_BG_LAYERS) {
      BackgroundLayer layer;
      if (parse_repeating_gradient(ss, item, &layer))
        st->bg_layers[st->bg_layer_count++] = layer;
    }
  } else if (strcmp(prop, "box-shadow") == 0) {
    parse_shadow_list(ss, val, true, st->box_shadows, &st->box_shadow_count,
                      &st->box_shadow_set);
  } else if (strcmp(prop, "border-radius") == 0) {
    st->radius = parse_int(val);
    st->radius_set = true;
  } else if (strcmp(prop, "min-width") == 0) {
    st->min_width = parse_int(val);
    st->min_width_set = true;
  } else if (strcmp(prop, "padding") == 0) {
    int a, b;
    parse_int_pair(val, &a, &b);
    st->padding_left = st->padding_right = b;
    st->padding_left_set = st->padding_right_set = true;
  } else if (strcmp(prop, "padding-left") == 0) {
    st->padding_left = parse_int(val);
    st->padding_left_set = true;
  } else if (strcmp(prop, "padding-right") == 0) {
    st->padding_right = parse_int(val);
    st->padding_right_set = true;
  } else if (strcmp(prop, "margin") == 0) {
    int a, b;
    parse_int_pair(val, &a, &b);
    st->margin_top = a;
    st->margin_top_set = true;
    st->margin_left = st->margin_right = b;
    st->margin_left_set = st->margin_right_set = true;
  } else if (strcmp(prop, "margin-top") == 0) {
    st->margin_top = parse_int(val);
    st->margin_top_set = true;
  } else if (strcmp(prop, "margin-left") == 0) {
    st->margin_left = parse_int(val);
    st->margin_left_set = true;
  } else if (strcmp(prop, "margin-right") == 0) {
    st->margin_right = parse_int(val);
    st->margin_right_set = true;
  } else if (strcmp(prop, "font-family") == 0) {
    if (val[0]) {
      // Take only the first family name, strip quotes and fallbacks
      const char *p = val;
      while (*p && isspace((unsigned char)*p))
        p++;
      char fam[sizeof(ss->font_family)];
      size_t n = 0;
      bool quoted = false;
      if (*p == '"' || *p == '\'') {
        quoted = true;
        p++;
      }
      while (*p && n + 1 < sizeof(fam)) {
        if (quoted && (*p == '"' || *p == '\'')) {
          p++;
          break;
        }
        if (!quoted && *p == ',')
          break;
        fam[n++] = *p++;
      }
      while (n > 0 && isspace((unsigned char)fam[n - 1]))
        n--;
      fam[n] = '\0';
      if (fam[0]) {
        strncpy(ss->font_family, fam, sizeof(ss->font_family) - 1);
        ss->font_family[sizeof(ss->font_family) - 1] = '\0';
        ss->font_set = true;
      }
    }
  } else if (strcmp(prop, "font-size") == 0) {
    ss->font_size = parse_int(val);
    if (ss->font_size > 0)
      ss->font_set = true;
  } else if (strcmp(prop, "font-weight") == 0) {
    if (val[0]) {
      strncpy(ss->font_weight, val, sizeof(ss->font_weight) - 1);
      ss->font_weight[sizeof(ss->font_weight) - 1] = '\0';
      ss->font_set = true;
    }
  }
  // Ignore: font shorthand, border, border-radius, opacity, etc.
}

void style_sheet_init(StyleSheet *ss) {
  memset(ss, 0, sizeof(*ss));
}

// Parse a selector string (with optional state) into module / state
static void parse_selector(const char *sel, char *module, size_t msz,
                           char *state, size_t ssz) {
  module[0] = state[0] = '\0';
  const char *p = sel;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p == '*') {
    strncpy(module, "*", msz - 1);
    p++;
  } else if (*p == '#') {
    p++;
    const char *q = p;
    while (*q && *q != '.' && *q != ':' && !isspace((unsigned char)*q) &&
           *q != '{')
      q++;
    size_t n = (size_t)(q - p);
    if (n >= msz)
      n = msz - 1;
    memcpy(module, p, n);
    module[n] = '\0';
    p = q;
  } else if (strncmp(p, "menuitem", 8) == 0) {
    strncpy(module, "menuitem", msz - 1);
    p += 8;
    while (*p && isspace((unsigned char)*p))
      p++;
    if (*p == ':') {
      p++;
      const char *q = p;
      while (*q && !isspace((unsigned char)*q) && *q != '{' && *q != '}')
        q++;
      size_t n = (size_t)(q - p);
      if (n >= ssz)
        n = ssz - 1;
      memcpy(state, p, n);
      state[n] = '\0';
    }
    return;
  } else if (strncmp(p, "menu", 4) == 0) {
    strncpy(module, "menu", msz - 1);
    return;
  }
  // Skip child prefixes like "button"
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p == '>') {
    // Child rules like #tray > .passive don't apply to the module
    module[0] = '\0';
    return;
  }
  if (strncmp(p, "button", 6) == 0)
    p += 6;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p == '.') {
    p++;
    const char *q = p;
    while (*q && !isspace((unsigned char)*q) && *q != '{' && *q != '}')
      q++;
    size_t n = (size_t)(q - p);
    if (n >= ssz)
      n = ssz - 1;
    memcpy(state, p, n);
    state[n] = '\0';
  } else if (*p == ':') {
    p++;
    const char *q = p;
    while (*q && !isspace((unsigned char)*q) && *q != '{' && *q != '}')
      q++;
    size_t n = (size_t)(q - p);
    if (n >= ssz)
      n = ssz - 1;
    memcpy(state, p, n);
    state[n] = '\0';
  }
}

int style_sheet_parse(StyleSheet *ss, const char *buf) {
  const char *p = buf;
  for (;;) {
    skip_ws_comments(&p);
    if (!*p)
      break;

    // @define-color name value;
    if (strncmp(p, "@define-color", 13) == 0) {
      p += 13;
      while (*p && isspace((unsigned char)*p))
        p++;
      char name[64];
      size_t nl = 0;
      while (*p && !isspace((unsigned char)*p) && *p != ';' &&
             nl + 1 < sizeof(name))
        name[nl++] = *p++;
      name[nl] = '\0';
      while (*p && isspace((unsigned char)*p))
        p++;
      char val[64];
      size_t vl = 0;
      while (*p && *p != ';' && vl + 1 < sizeof(val))
        val[vl++] = *p++;
      val[vl] = '\0';
      if (*p == ';')
        p++;
      if (nl && vl && ss->var_count < MANGOBAR_MAX_VARS) {
        snprintf(ss->var_names[ss->var_count], sizeof(ss->var_names[0]), "%s",
                 name);
        snprintf(ss->var_values[ss->var_count], sizeof(ss->var_values[0]),
                 "%s", val);
        ss->var_count++;
      }
      continue;
    }

    // Skip @keyframes ... { ... } blocks
    if (strncmp(p, "@keyframes", 10) == 0) {
      while (*p && *p != '{')
        p++;
      if (*p == '{') {
        int depth = 0;
        while (*p) {
          if (*p == '{')
            depth++;
          else if (*p == '}' && --depth <= 0) {
            p++;
            break;
          }
          p++;
        }
      }
      continue;
    }

    // Selector list
    char selector[2048];
    size_t sl = 0;
    while (*p && *p != '{' && sl < sizeof(selector) - 1)
      selector[sl++] = *p++;
    selector[sl] = '\0';
    if (*p == '{')
      p++;
    bool is_menu_rule = strstr(selector, "menu") != NULL;

    // Declaration block
    Style decl = {0};
    for (;;) {
      skip_ws_comments(&p);
      if (!*p)
        break;
      if (*p == '}') {
        p++;
        break;
      }
      char prop[64];
      size_t pl = 0;
      while (*p && *p != ':' && *p != ';' && *p != '}' &&
             pl < sizeof(prop) - 1)
        prop[pl++] = *p++;
      prop[pl] = '\0';
      while (*p && *p != ':' && *p != ';' && *p != '}')
        p++;
      if (*p == ':')
        p++;
      skip_ws_comments(&p);
      char val[512];
      size_t vl = 0;
      while (*p && *p != ';' && *p != '}' && vl < sizeof(val) - 1)
        val[vl++] = *p++;
      val[vl] = '\0';
      char *vp = val;
      while (*vp && isspace((unsigned char)*vp))
        vp++;
      char *ve = val + vl;
      while (ve > vp && isspace((unsigned char)ve[-1]))
        ve--;
      *ve = '\0';
      if (*p == ';')
        p++;
      if (prop[0]) {
        if (is_menu_rule && strcmp(prop, "font-size") == 0)
          ss->menu_font_size = parse_int(vp);
        else
          apply_decl(ss, &decl, prop, vp);
      }
    }

    // Split comma-separated selectors
    char *cs = strdup(selector);
    if (!cs)
      continue;
    char *tok = strtok(cs, ",");
    while (tok && ss->rule_count < MANGOBAR_MAX_STYLE_RULES) {
      StyleRule *r = &ss->rules[ss->rule_count++];
      parse_selector(tok, r->module, sizeof(r->module), r->state,
                     sizeof(r->state));
      r->style = decl;
      if (strcmp(r->module, "menu") == 0 && r->state[0] == '\0') {
        apply_rule(&ss->menu, r);
        if (r->style.radius_set)
          ss->menu_radius = r->style.radius;
      } else if (strcmp(r->module, "menuitem") == 0 &&
                 r->state[0] == '\0') {
        apply_rule(&ss->menuitem, r);
      } else if (strcmp(r->module, "menuitem") == 0 &&
                 strcmp(r->state, "hover") == 0) {
        apply_rule(&ss->menuitem_hover, r);
      }
      tok = strtok(NULL, ",");
    }
    free(cs);
  }
  ss->loaded = true;
  return 0;
}

int style_sheet_load(StyleSheet *ss, const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    fclose(f);
    return -1;
  }
  char *buf = malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return -1;
  }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  buf[rd] = '\0';
  fclose(f);
  int ret = style_sheet_parse(ss, buf);
  free(buf);
  return ret;
}

static void apply_rule(Style *dst, const StyleRule *r) {
  const Style *s = &r->style;
  if (s->color_set)
    dst->color = s->color;
  if (s->background_set) {
    dst->background = s->background;
    dst->gradient_end = s->gradient_end;
    dst->gradient_dir = s->gradient_dir;
    dst->bg_gradient = s->bg_gradient;
  }
  if (s->border_color_set)
    dst->border_color = s->border_color;
  if (s->border_width_set)
    dst->border_width = s->border_width;
  if (s->text_shadow_set) {
    memcpy(dst->text_shadows, s->text_shadows, sizeof(dst->text_shadows));
    dst->text_shadow_count = s->text_shadow_count;
  }
  if (s->box_shadow_set) {
    memcpy(dst->box_shadows, s->box_shadows, sizeof(dst->box_shadows));
    dst->box_shadow_count = s->box_shadow_count;
  }
  if (s->bg_layers_set) {
    memcpy(dst->bg_layers, s->bg_layers, sizeof(dst->bg_layers));
    dst->bg_layer_count = s->bg_layer_count;
  }
  if (s->padding_left_set)
    dst->padding_left = s->padding_left;
  if (s->padding_right_set)
    dst->padding_right = s->padding_right;
  if (s->margin_top_set)
    dst->margin_top = s->margin_top;
  if (s->margin_left_set)
    dst->margin_left = s->margin_left;
  if (s->margin_right_set)
    dst->margin_right = s->margin_right;
  if (s->radius_set)
    dst->radius = s->radius;
  if (s->min_width_set)
    dst->min_width = s->min_width;
  dst->color_set |= s->color_set;
  dst->background_set |= s->background_set;
  dst->border_color_set |= s->border_color_set;
  dst->border_width_set |= s->border_width_set;
  dst->text_shadow_set |= s->text_shadow_set;
  dst->box_shadow_set |= s->box_shadow_set;
  dst->bg_layers_set |= s->bg_layers_set;
  dst->padding_left_set |= s->padding_left_set;
  dst->padding_right_set |= s->padding_right_set;
  dst->margin_top_set |= s->margin_top_set;
  dst->margin_left_set |= s->margin_left_set;
  dst->margin_right_set |= s->margin_right_set;
  dst->radius_set |= s->radius_set;
  dst->min_width_set |= s->min_width_set;
}

Style style_overlay(const Style *base, const Style *over) {
  Style d = *base;
  StyleRule fake = {.style = *over};
  apply_rule(&d, &fake);
  return d;
}

Style style_resolve(const StyleSheet *ss, const char *module,
                    const char *state) {
  Style s = {0};
  // 1. "*" rules
  for (int i = 0; i < ss->rule_count; i++) {
    const StyleRule *r = &ss->rules[i];
    if (strcmp(r->module, "*") == 0 &&
        (r->state[0] == '\0' ||
         (state && strcmp(r->state, state) == 0)))
      apply_rule(&s, r);
  }
  // 2. "#module"
  for (int i = 0; i < ss->rule_count; i++) {
    const StyleRule *r = &ss->rules[i];
    if (strcmp(r->module, module) == 0 && r->state[0] == '\0')
      apply_rule(&s, r);
  }
  // 3. "#module.state"
  if (state && state[0]) {
    for (int i = 0; i < ss->rule_count; i++) {
      const StyleRule *r = &ss->rules[i];
      if (strcmp(r->module, module) == 0 &&
          strcmp(r->state, state) == 0)
        apply_rule(&s, r);
    }
  }
  return s;
}

Style style_resolve_module_only(const StyleSheet *ss, const char *module,
                                const char *state) {
  Style s = {0};
  for (int i = 0; i < ss->rule_count; i++) {
    const StyleRule *r = &ss->rules[i];
    if (strcmp(r->module, module) == 0 && r->state[0] == '\0')
      apply_rule(&s, r);
  }
  if (state && state[0]) {
    for (int i = 0; i < ss->rule_count; i++) {
      const StyleRule *r = &ss->rules[i];
      if (strcmp(r->module, module) == 0 && strcmp(r->state, state) == 0)
        apply_rule(&s, r);
    }
  }
  return s;
}

Style style_resolve_state(const StyleSheet *ss, const char *module,
                          const char *state) {
  Style s = {0};
  for (int pass = 0; pass < 2; pass++) {
    const char *name = pass == 0 ? "*" : module;
    for (int i = 0; i < ss->rule_count; i++) {
      const StyleRule *r = &ss->rules[i];
      if (strcmp(r->module, name) == 0 && strcmp(r->state, state) == 0)
        apply_rule(&s, r);
    }
  }
  return s;
}

const char *style_font_string(const StyleSheet *ss, const char *fallback) {
  if (!ss->font_set || !ss->font_family[0])
    return fallback;
  static char buf[256];
  const char *weight = ss->font_weight[0] ? ss->font_weight : "Regular";
  int size = ss->font_size > 0 ? ss->font_size : 12;
  // fcft format: family:style=weight:size=N
  snprintf(buf, sizeof(buf), "%s:style=%s:size=%d", ss->font_family, weight,
           size);
  return buf;
}

int style_find_default_path(char *out, size_t outsz) {
  // 1. $MANGOBAR_CSS
  const char *env = getenv("MANGOBAR_CSS");
  if (env && *env && access(env, R_OK) == 0) {
    strncpy(out, env, outsz - 1);
    out[outsz - 1] = '\0';
    return 0;
  }
  // 2. $XDG_CONFIG_HOME/mangobar/style.css
  const char *xdg = getenv("XDG_CONFIG_HOME");
  char path[512];
  if (xdg && *xdg) {
    snprintf(path, sizeof(path), "%s/mangobar/style.css", xdg);
    if (access(path, R_OK) == 0) {
      strncpy(out, path, outsz - 1);
      out[outsz - 1] = '\0';
      return 0;
    }
  }
  // 3. ~/.config/mangobar/style.css
  const char *home = getenv("HOME");
  if (home && *home) {
    snprintf(path, sizeof(path), "%s/.config/mangobar/style.css", home);
    if (access(path, R_OK) == 0) {
      strncpy(out, path, outsz - 1);
      out[outsz - 1] = '\0';
      return 0;
    }
  }
  return -1;
}

int style_sheet_shadow_reach(const StyleSheet *ss) {
  int reach = 0;
  for (int i = 0; i < ss->rule_count; i++) {
    const Style *s = &ss->rules[i].style;
    for (int j = 0; j < s->box_shadow_count; j++) {
      const Shadow *sh = &s->box_shadows[j];
      int r = sh->y + sh->spread + sh->blur;
      if (!sh->inset && r > reach)
        reach = r;
    }
  }
  return reach;
}
