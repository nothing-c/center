/*
 * BSD Zero Clause License
 *
 * Copyright (c) 2022 Thomas Voss
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#define POSIXLY_CORRECT
#define _POSIX_C_SOURCE 200809L

#include <sys/ioctl.h>
#include <sys/queue.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ESC 033

struct line_item {
	char *buffer;
	STAILQ_ENTRY(line_item) list;
};

STAILQ_HEAD(lines_head, line_item);

static void center(FILE *);
static void center_by_longest(FILE *);
static int cols(void);
static int utf8len(const char *);
static int noesclen(const char *);
static int matchesc(const char *);

extern int optind;
extern char *optarg;

int rval;
long width;
long tabwidth = 8;
int (*lenfunc)(const char *) = noesclen;

int
main(int argc, char **argv)
{
	int opt;
	char *endptr;
	void (*centerfunc)(FILE *) = center;

	while ((opt = getopt(argc, argv, ":elsw:t:")) != -1) {
		switch (opt) {
		case 'e':
			lenfunc = utf8len;
			break;
		case 'w':
			width = strtol(optarg, &endptr, 0);
			if (*optarg == '\0' || *endptr != '\0')
				errx(EXIT_FAILURE, "Invalid integer '%s'", optarg);
			if (width <= 0)
				errx(EXIT_FAILURE, "Width must be >0");
			if (errno == ERANGE || width > INT_MAX)
				warnx("Potential overflow of given width");
			break;
		case 't':
			tabwidth = strtol(optarg, &endptr, 0);
			if (*optarg == '\0' || *endptr != '\0')
				errx(EXIT_FAILURE, "Invalid integer '%s'", optarg);
			if (tabwidth < 0)
				errx(EXIT_FAILURE, "Tab width must be >=0");
			if (errno == ERANGE || tabwidth > INT_MAX)
				warnx("Potential overflow of given tab size");
			break;	
		case 'l':
			centerfunc = center_by_longest;
			break;
		default:
			fprintf(stderr, "Usage: %s [-e] [-w width] [-t tab width] [file ...]\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (!width && (width = cols()) == -1)
		errx(EXIT_FAILURE, "Unable to determine output width");

	argc -= optind;
	argv += optind;

	if (argc == 0)
		centerfunc(stdin);
	else do {
		if (strcmp(*argv, "-") == 0)
			centerfunc(stdin);
		else {
			FILE *fp;
			if ((fp = fopen(*argv, "r")) == NULL) {
				warn("fopen");
				rval = EXIT_FAILURE;
			} else {
				centerfunc(fp);
				fclose(fp);
			}
		}
	} while (*++argv);

	return rval;
}

/* Write the contents of the file pointed to by `fp' center aligned to the standard output */
void
center(FILE *fp)
{
	static char *line = NULL;
	static size_t bs = 0;

	while (getline(&line, &bs, fp) != -1) {
		int len, tabs = 0;
		char *match = line;

		while (strchr(match, '\t')) {
			match++;
			tabs++;
		}

		len = lenfunc(line) + tabs * tabwidth - tabs;
		for (int i = (width - len) / 2; i >= 0; i--)
			putchar(' ');
		fputs(line, stdout);
	}
	if (ferror(fp)) {
		warn("getline");
		rval = EXIT_FAILURE;
	}
}

/* Same as center(), but aligns all lines according to the longest line.
 * Great for centering code.
 */
void
center_by_longest(FILE *fp)
{
	static char *buffer = NULL;
	static size_t bs = 0;
	struct lines_head list_head;
	struct line_item *line;
	struct line_item *tmp;
	size_t longest = 0;
	size_t curr_len;

	STAILQ_INIT(&list_head);

	/* Collect all input lines in a list and find the longest */
	while (getline(&buffer, &bs, fp) != -1) {
		line = malloc(sizeof(struct line_item));
		if (!line) {
			warn("malloc");
			rval = EXIT_FAILURE;
			return;
		}

		line->buffer = buffer;
		STAILQ_INSERT_TAIL(&list_head, line, list);
		curr_len = lenfunc(buffer);
		if (curr_len > longest)
			longest = curr_len;

		/* Reset buffer and bs so getline can allocate a new buffer next time */
		buffer = NULL;
		bs = 0;
	}

	if (ferror(fp)) {
		warn("getline");
		rval = EXIT_FAILURE;
		return;
	}

	/* Output lines aligned to the longest and free them */
	line = STAILQ_FIRST(&list_head);
	while (line != NULL) {
		int len = longest;
		for (int i = (width - len) / 2; i >= 0; i--)
			putchar(' ');
		fputs(line->buffer, stdout);

		tmp = STAILQ_NEXT(line, list);
		free(line->buffer);
		free(line);
		line = tmp;
	}
}

/* Return the column width of the output device if it is a TTY. If it is not a TTY then return -1 */
int
cols(void)
{
	struct winsize w;

	if (!isatty(STDOUT_FILENO))
		return -1;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1)
		err(EXIT_FAILURE, "ioctl");

	return w.ws_col;
}

/* Return the length of the UTF-8 encoded string `s' in characters, not bytes */
int
utf8len(const char *s)
{
	int l = 0;

	while (*s)
		l += (*s++ & 0xC0) != 0x80;

	if (l > 0 && s[-1] == '\n')
		l--;

	return l;
}

/* Return the length of the UTF-8 encoded string `s' in characters, not bytes. Additionally this
 * function ignores ANSI color escape sequences when computing the length.
 */
int
noesclen(const char *s)
{
	int off = 0;
	const char *d = s;

	while ((d = strchr(d, ESC)) != NULL)
		off += matchesc(++d);
	
	return utf8len(s) - off;
}

/* Return the length of the ANSI color escape sequence at the beginning of the string `s'. If no
 * escape sequence is matched then 0 is returned. The local variable `c' is initialized to 3 in order
 * to account for the leading ESC, the following `[', and the trailing `m'.
 */
int
matchesc(const char *s)
{
	int c = 3;
	
	if (*s++ != '[')
		return 0;
	while (isdigit(*s) || *s == ';') {
		c++;
		s++;
	}
	
	return *s == 'm' ? c : 0;
}
