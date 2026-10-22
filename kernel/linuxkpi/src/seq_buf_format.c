/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux lib/seq_buf.c formatting subset. Full import pulls in Linux VFS
 * path formatting, which the native VFS does not implement. */
#include <linux/bug.h>
#include <linux/seq_buf.h>
#include <linux/sprintf.h>
#include <linux/stdarg.h>

int seq_buf_vprintf(struct seq_buf *s, const char *fmt, va_list args)
{
	int len;

	WARN_ON(s->size == 0);

	if (s->len < s->size) {
		len = vsnprintf(s->buffer + s->len, s->size - s->len, fmt, args);
		if (s->len + len < s->size) {
			s->len += len;
			return 0;
		}
	}
	seq_buf_set_overflow(s);
	return -1;
}

int seq_buf_printf(struct seq_buf *s, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = seq_buf_vprintf(s, fmt, ap);
	va_end(ap);

	return ret;
}
