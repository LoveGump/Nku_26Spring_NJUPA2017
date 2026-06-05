#include <klib.h>

static int out_char(char *s, size_t n, int *pos, char c) {
  if (n > 0 && (size_t)*pos + 1 < n) {
    s[*pos] = c;
  }
  (*pos)++;
  return 1;
}

static int out_str(char *s, size_t n, int *pos, const char *p) {
  int cnt = 0;
  if (p == NULL) p = "(null)";
  while (*p) {
    out_char(s, n, pos, *p++);
    cnt++;
  }
  return cnt;
}

static void reverse(char *s, int n) {
  for (int i = 0, j = n - 1; i < j; i++, j--) {
    char t = s[i];
    s[i] = s[j];
    s[j] = t;
  }
}

static int out_uint(char *s, size_t n, int *pos, unsigned int val, int base, int upper, int width, int zero) {
  char buf[32];
  int len = 0;
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

  do {
    buf[len++] = digits[val % base];
    val /= base;
  } while (val);
  while (len < width) {
    buf[len++] = zero ? '0' : ' ';
  }
  reverse(buf, len);
  for (int i = 0; i < len; i++) out_char(s, n, pos, buf[i]);
  return len;
}

static int out_int(char *s, size_t n, int *pos, int val, int width, int zero) {
  if (val < 0) {
    out_char(s, n, pos, '-');
    return 1 + out_uint(s, n, pos, (unsigned int)-val, 10, 0, width, zero);
  }
  return out_uint(s, n, pos, (unsigned int)val, 10, 0, width, zero);
}

static int out_float(char *s, size_t n, int *pos, double val, int precision) {
  if (precision < 0) precision = 3;
  if (val < 0) {
    out_char(s, n, pos, '-');
    val = -val;
  }

  int ip = (int)val;
  int cnt = out_uint(s, n, pos, (unsigned int)ip, 10, 0, 0, 0);
  out_char(s, n, pos, '.');
  cnt++;

  double frac = val - ip;
  for (int i = 0; i < precision; i++) {
    frac *= 10.0;
    int d = (int)frac;
    out_char(s, n, pos, '0' + d);
    frac -= d;
    cnt++;
  }
  return cnt;
}

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap) {
  int pos = 0;

  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      out_char(s, n, &pos, *fmt);
      continue;
    }

    fmt++;
    if (*fmt == '%') {
      out_char(s, n, &pos, '%');
      continue;
    }

    int zero = 0, width = 0, precision = -1;
    if (*fmt == '0') {
      zero = 1;
      fmt++;
    }
    while (*fmt >= '0' && *fmt <= '9') {
      width = width * 10 + (*fmt++ - '0');
    }
    if (*fmt == '.') {
      precision = 0;
      fmt++;
      while (*fmt >= '0' && *fmt <= '9') {
        precision = precision * 10 + (*fmt++ - '0');
      }
    }

    switch (*fmt) {
      case 'd':
      case 'i': out_int(s, n, &pos, va_arg(ap, int), width, zero); break;
      case 'u': out_uint(s, n, &pos, va_arg(ap, unsigned int), 10, 0, width, zero); break;
      case 'x': out_uint(s, n, &pos, va_arg(ap, unsigned int), 16, 0, width, zero); break;
      case 'X': out_uint(s, n, &pos, va_arg(ap, unsigned int), 16, 1, width, zero); break;
      case 'p':
        out_str(s, n, &pos, "0x");
        out_uint(s, n, &pos, (unsigned int)va_arg(ap, void *), 16, 0, width, zero);
        break;
      case 'c': out_char(s, n, &pos, (char)va_arg(ap, int)); break;
      case 's': out_str(s, n, &pos, va_arg(ap, const char *)); break;
      case 'f': out_float(s, n, &pos, va_arg(ap, double), precision); break;
      default:
        out_char(s, n, &pos, '%');
        out_char(s, n, &pos, *fmt);
        break;
    }
  }

  if (n > 0) {
    s[(pos < (int)n) ? pos : (int)n - 1] = '\0';
  }
  return pos;
}

static int skip_space(const char **p) {
  int skipped = 0;
  while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') {
    (*p)++;
    skipped = 1;
  }
  return skipped;
}

static int scan_int(const char **p, int base, unsigned int *out) {
  unsigned int val = 0;
  int n = 0;
  skip_space(p);
  while (1) {
    char c = **p;
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else break;
    if (d >= base) break;
    val = val * base + d;
    (*p)++;
    n++;
  }
  *out = val;
  return n > 0;
}

int sscanf(const char *str, const char *fmt, ...) {
  va_list ap;
  int matched = 0;
  va_start(ap, fmt);

  while (*fmt) {
    if (*fmt != '%') {
      if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
        skip_space(&str);
        fmt++;
        continue;
      }
      if (*str++ != *fmt++) break;
      continue;
    }

    fmt++;
    while (*fmt >= '0' && *fmt <= '9') fmt++;

    if (*fmt == 'd' || *fmt == 'i') {
      int neg = 0;
      unsigned int val;
      skip_space(&str);
      if (*str == '-') {
        neg = 1;
        str++;
      }
      if (!scan_int(&str, 10, &val)) break;
      *va_arg(ap, int *) = neg ? -(int)val : (int)val;
      matched++;
    } else if (*fmt == 'u') {
      unsigned int val;
      if (!scan_int(&str, 10, &val)) break;
      *va_arg(ap, unsigned int *) = val;
      matched++;
    } else if (*fmt == 'x' || *fmt == 'X') {
      unsigned int val;
      if (!scan_int(&str, 16, &val)) break;
      *va_arg(ap, unsigned int *) = val;
      matched++;
    } else if (*fmt == 'f') {
      int neg = 0;
      float val = 0.0f, scale = 0.1f;
      skip_space(&str);
      if (*str == '-') {
        neg = 1;
        str++;
      }
      while (*str >= '0' && *str <= '9') {
        val = val * 10.0f + (*str++ - '0');
      }
      if (*str == '.') {
        str++;
        while (*str >= '0' && *str <= '9') {
          val += (*str++ - '0') * scale;
          scale *= 0.1f;
        }
      }
      *va_arg(ap, float *) = neg ? -val : val;
      matched++;
    }
    fmt++;
  }

  va_end(ap);
  return matched;
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
  char *a = (char *)base;
  for (size_t i = 1; i < nmemb; i++) {
    for (size_t j = i; j > 0 && compar(a + j * size, a + (j - 1) * size) < 0; j--) {
      for (size_t k = 0; k < size; k++) {
        char t = a[j * size + k];
        a[j * size + k] = a[(j - 1) * size + k];
        a[(j - 1) * size + k] = t;
      }
    }
  }
}

const char *strchr(const char *s, int c) {
  while (*s) {
    if (*s == c) return s;
    s++;
  }
  return c == 0 ? s : NULL;
}

char *strstr(const char *s, const char *needle) {
  if (*needle == 0) return (char *)s;
  for (; *s; s++) {
    const char *a = s, *b = needle;
    while (*a && *b && *a == *b) {
      a++;
      b++;
    }
    if (*b == 0) return (char *)s;
  }
  return NULL;
}

int toupper(int c) {
  return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

int isprint(int c) {
  return c >= 32 && c < 127;
}

float fabsf(float x) {
  return x < 0.0f ? -x : x;
}

float floorf(float x) {
  int i = (int)x;
  return (x < 0.0f && (float)i != x) ? (float)(i - 1) : (float)i;
}

float ceilf(float x) {
  int i = (int)x;
  return (x > 0.0f && (float)i != x) ? (float)(i + 1) : (float)i;
}

float fmodf(float x, float y) {
  if (y == 0.0f) return 0.0f;
  int q = (int)(x / y);
  return x - (float)q * y;
}

float sqrtf(float x) {
  if (x <= 0.0f) return 0.0f;
  float r = x > 1.0f ? x : 1.0f;
  for (int i = 0; i < 12; i++) {
    r = 0.5f * (r + x / r);
  }
  return r;
}

static float wrap_pi(float x) {
  const float pi = 3.1415926f;
  const float two_pi = 6.2831852f;
  while (x > pi) x -= two_pi;
  while (x < -pi) x += two_pi;
  return x;
}

float sinf(float x) {
  x = wrap_pi(x);
  float x2 = x * x;
  return x * (1.0f - x2 / 6.0f + x2 * x2 / 120.0f - x2 * x2 * x2 / 5040.0f);
}

float cosf(float x) {
  x = wrap_pi(x);
  float x2 = x * x;
  return 1.0f - x2 / 2.0f + x2 * x2 / 24.0f - x2 * x2 * x2 / 720.0f;
}

static float exp_approx(float x) {
  float term = 1.0f, sum = 1.0f;
  for (int i = 1; i < 16; i++) {
    term *= x / (float)i;
    sum += term;
  }
  return sum;
}

static float log_approx(float x) {
  if (x <= 0.0f) return 0.0f;
  float y = (x - 1.0f) / (x + 1.0f);
  float y2 = y * y;
  float term = y, sum = 0.0f;
  for (int i = 1; i < 20; i += 2) {
    sum += term / (float)i;
    term *= y2;
  }
  return 2.0f * sum;
}

float powf(float x, float y) {
  if (y == 0.0f) return 1.0f;
  if (y == 1.0f) return x;
  if (y == 2.0f) return x * x;
  if (y == 0.5f) return sqrtf(x);
  return exp_approx(y * log_approx(x));
}
