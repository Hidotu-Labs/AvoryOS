/* LinuxKPI module parameters and __setup() handlers.
 * See linux/moduleparam.h and kernel/linker-scripts/x86_64.lds. */

#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native.h>

extern const struct kpi_param __kpi_param_start[], __kpi_param_end[];
extern const struct kpi_setup __kpi_setup_start[], __kpi_setup_end[];

#define KPI_PARSE_BUF 256

static long kpi_parse_long(const char *s, bool *ok) {
  unsigned long v = 0;
  bool neg = false;
  bool any = false;

  *ok = false;
  while (*s == ' ' || *s == '\t')
    s++;

  if (*s == '-') {
    neg = true;
    s++;
  } else if (*s == '+') {
    s++;
  }

  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
    while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
           (*s >= 'A' && *s <= 'F')) {
      unsigned int d = (*s <= '9')   ? (unsigned int)(*s - '0')
                       : (*s >= 'a') ? (unsigned int)(*s - 'a' + 10)
                                     : (unsigned int)(*s - 'A' + 10);
      v = v * 16 + d;
      s++;
      any = true;
    }
  } else {
    while (*s >= '0' && *s <= '9') {
      v = v * 10 + (unsigned int)(*s - '0');
      s++;
      any = true;
    }
  }

  if (!any)
    return 0;
  *ok = true;
  return neg ? -(long)v : (long)v;
}

static char *kpi_kstrdup(const char *s) {
  size_t len = strlen(s) + 1;
  char *p = kmalloc(len, GFP_KERNEL);

  if (p)
    memcpy(p, s, len);
  return p;
}

static void param_set_value(const struct kpi_param *p, const char *val) {
  bool ok;
  long l;

  if (!val)
    val = "";

  switch (p->type) {
  case KPI_PARAM_bool:
    *(bool *)p->value = !(val[0] == '0' || val[0] == 'n' || val[0] == 'N');
    break;
  case KPI_PARAM_invbool:
    *(bool *)p->value = (val[0] == '0' || val[0] == 'n' || val[0] == 'N');
    break;
  case KPI_PARAM_byte:
    l = kpi_parse_long(val, &ok);
    if (ok)
      *(s8 *)p->value = (s8)l;
    break;
  case KPI_PARAM_short:
    l = kpi_parse_long(val, &ok);
    if (ok)
      *(s16 *)p->value = (s16)l;
    break;
  case KPI_PARAM_ushort:
    l = kpi_parse_long(val, &ok);
    if (ok)
      *(u16 *)p->value = (u16)l;
    break;
  case KPI_PARAM_int:
    l = kpi_parse_long(val, &ok);
    if (ok)
      *(int *)p->value = (int)l;
    break;
  case KPI_PARAM_uint:
    l = kpi_parse_long(val, &ok);
    if (ok)
      *(unsigned int *)p->value = (unsigned int)l;
    break;
  case KPI_PARAM_long:
    l = kpi_parse_long(val, &ok);
    if (ok)
      *(long *)p->value = l;
    break;
  case KPI_PARAM_ulong:
    l = kpi_parse_long(val, &ok);
    if (ok)
      *(unsigned long *)p->value = (unsigned long)l;
    break;
  case KPI_PARAM_ullong:
    l = kpi_parse_long(val, &ok);
    if (ok)
      *(unsigned long long *)p->value = (unsigned long long)l;
    break;
  case KPI_PARAM_bint:
    l = kpi_parse_long(val, &ok);
    if (ok)
      *(int *)p->value = (int)l;
    break;
  case KPI_PARAM_hexint:
    l = kpi_parse_long(val, &ok);
    if (ok)
      *(unsigned int *)p->value = (unsigned int)l;
    break;
  case KPI_PARAM_charp:
    if (*(char **)p->value)
      kfree(*(char **)p->value);
    *(char **)p->value = kpi_kstrdup(val);
    break;
  case KPI_PARAM_string:
    if (p->maxlen) {
      strncpy((char *)p->value, val, p->maxlen - 1);
      ((char *)p->value)[p->maxlen - 1] = '\0';
    }
    break;
  default:
    break;
  }
}

/* Match `token` against a setup string; return the argument (text after an
 * optional '=') or NULL when it does not match.  token_dispatch() has already
 * split "key=value" at the '=', so a setup string ending in '=' matches the
 * bare key and the value is passed by the caller. */
static const char *setup_match(const char *token, const char *str) {
  size_t n = strlen(str);

  if (n && str[n - 1] == '=') {
    if (strncmp(token, str, n - 1) != 0 || token[n - 1] != '\0')
      return NULL;
    return token + n - 1;
  }

  if (strncmp(token, str, n) != 0)
    return NULL;
  if (token[n] == '\0')
    return token + n;
  if (token[n] == '=')
    return token + n + 1;
  return NULL;
}

static void token_dispatch(char *token) {
  char *eq = NULL;
  const char *val = NULL;
  const char *dot;

  for (char *q = token; *q; q++) {
    if (*q == '=') {
      eq = q;
      break;
    }
  }

  if (eq) {
    *eq = '\0';
    val = eq + 1;
  }

  for (const struct kpi_setup *s = __kpi_setup_start; s < __kpi_setup_end;
       s++) {
    const char *arg = setup_match(token, s->str);

    if (arg) {
      s->fn((char *)(eq ? val : arg));
      return;
    }
  }

  for (const struct kpi_param *p = __kpi_param_start; p < __kpi_param_end;
       p++) {
    if (strcmp(token, p->name) == 0) {
      param_set_value(p, val);
      return;
    }
  }

  /* Accept the upstream `module.param=value` spelling.  The param table has
   * no per-module scoping (KBUILD_MODNAME is a single constant for the whole
   * kernel here), so after an exact miss retry with the suffix following the
   * last '.'.  Param names must therefore stay unique across the linked
   * modules; the table is small and audited (drm.debug, amdgpu.runpm, ...).
   * Phase 6 C3 addition; see docs/linuxkpi-gaps.md. */
  dot = strrchr(token, '.');
  if (dot && dot[1]) {
    for (const struct kpi_param *p = __kpi_param_start; p < __kpi_param_end;
         p++) {
      if (strcmp(dot + 1, p->name) == 0) {
        param_set_value(p, val);
        return;
      }
    }
  }
}

void linuxkpi_param_parse(const char *cmdline) {
  char buf[KPI_PARSE_BUF];
  size_t n = 0;

  if (!cmdline)
    return;

  for (const char *p = cmdline;; p++) {
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\0') {
      if (n) {
        buf[n] = '\0';
        token_dispatch(buf);
        n = 0;
      }
      if (*p == '\0')
        break;
      continue;
    }
    if (n + 1 < sizeof(buf))
      buf[n++] = *p;
  }
}

void linuxkpi_param_init(void) {
  const char *cmdline = linuxkpi_boot_cmdline();

  if (cmdline && cmdline[0]) {
    klogf("[LINUXKPI] cmdline: %s\n", cmdline);
    linuxkpi_param_parse(cmdline);
  }
}
