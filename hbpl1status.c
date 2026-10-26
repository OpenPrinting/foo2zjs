/*
 * hbpl1status - report the status of HBPLv1 printers (Dell 1250c,
 * Xerox Phaser 6000, Fuji Xerox DocuPrint CP105b, ...) from the STS field
 * of their IEEE 1284 device ID.
 *
 * The printer re-reads the device ID live, and its STS field carries a
 * base64-encoded status block: engine state, error code and the four toner
 * levels.  See README.hbpl1status for the layout and what is confirmed.
 *
 * Modes:
 *	hbpl1status [-c|-j] [device]	usblp device (Linux), default /dev/usb/lp0
 *	hbpl1status [-c|-j] -s		CUPS side channel (from a filter/wrapper)
 *	hbpl1status [-c|-j] -i ID	decode a device ID string
 *	hbpl1status job user title copies options [file]
 *					CUPS command filter (ReportLevels),
 *					installed as "command2foo2hbpl1"
 *
 * -c prints CUPS "ATTR:" and "STATE:" messages on stderr, -j prints JSON on
 * stdout, otherwise a human readable report is printed.
 *
 * Copyright 2026 Thibault
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <cups/cups.h>
#include <cups/sidechannel.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
    #include <sys/ioctl.h>
    #define IOCNR_GET_DEVICE_ID		1
    #define LPIOC_GET_DEVICE_ID(len) \
	_IOC(_IOC_READ, 'P', IOCNR_GET_DEVICE_ID, len)
#endif

#define DEVID_MAX	2048
#define STS_MAX		128
#define STS_MIN		24		/* Enough for the four toner levels */

/* Below this level (percent) a toner is reported as low */
#define LOW_LEVEL	15

/* Side channel timeouts, seconds */
#define SC_TIMEOUT_WRAPPER	5.0
#define SC_TIMEOUT_FILTER	30.0

enum { OUT_TEXT, OUT_CUPS, OUT_JSON };

typedef struct
{
    const char	*name;		/* marker-names */
    const char	*color;		/* marker-colors */
    int		offset;		/* Level byte; the flag byte follows */
} Toner;

/* CUPS marker order (K, C, M, Y); the printer stores them as Y, M, C, K */
static const Toner Toners[] =
{
    { "Black Toner",	"#000000",	22 },
    { "Cyan Toner",	"#00FFFF",	20 },
    { "Magenta Toner",	"#FF00FF",	18 },
    { "Yellow Toner",	"#FFFF00",	16 },
};
#define NTONERS	(int) (sizeof(Toners) / sizeof(Toners[0]))

/* Toner flag values observed on a Dell 1250c */
#define FLAG_PRESENT	0x23
#define FLAG_MISSING	0x20
#define LEVEL_UNKNOWN	0x7f

typedef struct
{
    int		level;		/* 0-100, or -1 when unknown */
    int		missing;
    int		low;
    int		empty;
} TonerState;

typedef struct
{
    char		model[256];
    unsigned char	sts[STS_MAX];
    int			len;
    int			state, substate;
    int			err_major, err_minor;	/* 0xff/0xff when no error */
    int			media_empty;
    int			cover_open;
    TonerState		toner[NTONERS];
    int			plausible;	/* Toner bytes match the known layout */
} Status;

static const char *Progname = "hbpl1status";

static void
usage(void)
{
    fprintf(stderr,
"Usage:\n"
"   %s [-c|-j] [device]	read the usblp device [/dev/usb/lp0]\n"
"   %s [-c|-j] -s		read through the CUPS side channel\n"
"   %s [-c|-j] -i device-id	decode a IEEE 1284 device ID string\n"
"   %s job user title copies options [file]\n"
"				CUPS command filter (ReportLevels)\n"
"\n"
"	Report the toner levels and state of an HBPLv1 printer\n"
"	(Dell 1250c, Xerox Phaser 6000, Fuji Xerox DocuPrint CP105b).\n"
"\n"
"Options:\n"
"-c	Print CUPS ATTR: and STATE: messages on stderr\n"
"-j	Print JSON on stdout\n"
"-i id	Decode this device ID instead of asking the printer\n"
"-s	Ask the CUPS backend for the device ID (CUPS_SC_CMD_GET_DEVICE_ID)\n",
	Progname, Progname, Progname, Progname);
    exit(1);
}

static int
b64value(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/*
 * Decode base64, skipping the white space (spaces, CR/LF) the printer
 * inserts to wrap the value.  Returns the number of bytes, -1 on error.
 */
static int
b64decode(const char *in, size_t inlen, unsigned char *out, int outmax)
{
    unsigned int	acc = 0;
    int			bits = 0, n = 0, v;
    size_t		i;

    for (i = 0; i < inlen; ++i)
    {
	if (isspace((unsigned char) in[i]))
	    continue;
	if (in[i] == '=')
	    break;
	if ((v = b64value((unsigned char) in[i])) < 0)
	    return -1;
	acc = (acc << 6) | v;
	bits += 6;
	if (bits >= 8)
	{
	    bits -= 8;
	    if (n >= outmax)
		return -1;
	    out[n++] = (acc >> bits) & 0xff;
	}
    }
    return n;
}

/*
 * Find "key:" in a device ID and copy its value.  The STS value may
 * contain anything but ';'.
 */
static int
devid_field(const char *devid, const char *key, char *val, size_t valsize,
	    size_t *vallen)
{
    const char	*p = devid, *end;
    size_t	klen = strlen(key);

    while (*p)
    {
	while (*p == ';' || isspace((unsigned char) *p))
	    ++p;
	if (strncmp(p, key, klen) == 0 && p[klen] == ':')
	{
	    p += klen + 1;
	    end = strchr(p, ';');
	    if (!end)
		end = p + strlen(p);
	    *vallen = end - p;
	    if (*vallen >= valsize)
		*vallen = valsize - 1;
	    memcpy(val, p, *vallen);
	    val[*vallen] = '\0';
	    return 0;
	}
	if (!(p = strchr(p, ';')))
	    break;
    }
    return -1;
}

static int
decode(const char *devid, Status *st)
{
    char	val[DEVID_MAX];
    size_t	vallen;
    int		i;

    memset(st, 0, sizeof(*st));
    if (devid_field(devid, "MDL", val, sizeof(st->model), &vallen) == 0
	|| devid_field(devid, "MODEL", val, sizeof(st->model), &vallen) == 0)
	strcpy(st->model, val);

    if (devid_field(devid, "STS", val, sizeof(val), &vallen) < 0)
    {
	fprintf(stderr, "DEBUG: %s: no STS field in device ID\n", Progname);
	return -1;
    }
    st->len = b64decode(val, vallen, st->sts, sizeof(st->sts));
    if (st->len < STS_MIN)
    {
	fprintf(stderr, "DEBUG: %s: STS field too short or invalid\n",
	    Progname);
	return -1;
    }

    st->state = st->sts[2];
    st->substate = st->sts[3];
    st->err_major = (st->sts[4] << 8) | st->sts[5];
    st->err_minor = (st->sts[6] << 8) | st->sts[7];
    st->media_empty = (st->err_major == 24 && st->err_minor == 969);
    st->cover_open = (st->err_major == 77 && st->err_minor == 300);

    st->plausible = 1;
    for (i = 0; i < NTONERS; ++i)
    {
	int		level = st->sts[Toners[i].offset];
	int		flag = st->sts[Toners[i].offset + 1];
	TonerState	*t = &st->toner[i];

	/*
	 * Other HBPLv1 models may use another layout: don't report numbers
	 * that don't look like ours.
	 */
	if ((level > 100 && level != LEVEL_UNKNOWN) || (flag & 0xe0) != 0x20)
	    st->plausible = 0;

	t->missing = (flag == FLAG_MISSING);
	t->level = (level > 100 || t->missing) ? -1 : level;
	t->empty = (t->level == 0);
	t->low = (t->level > 0 && t->level < LOW_LEVEL);
    }
    return 0;
}

static int
has_error(const Status *st)
{
    return !(st->err_major == 0xff && st->err_minor == 0xff);
}

/*
 * Error codes are shown by Dell/Fuji Xerox as "MMM-NNN".  Only the
 * meanings below were confirmed on a Dell 1250c.
 */
static const char *
error_text(const Status *st)
{
    if (!has_error(st))
	return NULL;
    if (st->err_major == 93 && st->err_minor >= 970 && st->err_minor <= 973)
	return "toner cartridge missing";
    if (st->media_empty)
	return "paper empty";
    if (st->cover_open)
	return "front cover open";
    if (st->err_major == 16 && st->err_minor == 799)
	return "job error";
    return "unknown error";
}

#ifdef __linux__
static int
read_usblp(const char *device, char *devid, size_t size)
{
    unsigned char	buf[DEVID_MAX];
    int			fd, len;

    /* O_NONBLOCK: never wait for the printer to become writable */
    if ((fd = open(device, O_RDONLY | O_NONBLOCK)) < 0)
    {
	fprintf(stderr, "%s: can't open '%s': %s\n",
	    Progname, device, strerror(errno));
	return -1;
    }
    if (ioctl(fd, LPIOC_GET_DEVICE_ID(sizeof(buf)), buf) < 0)
    {
	fprintf(stderr, "%s: GET_DEVICE_ID on '%s': %s\n",
	    Progname, device, strerror(errno));
	close(fd);
	return -1;
    }
    close(fd);
    len = (buf[0] << 8) + buf[1] - 2;
    if (len < 0 || len >= (int) size || len > (int) sizeof(buf) - 2)
	return -1;
    memcpy(devid, buf + 2, len);
    devid[len] = '\0';
    return 0;
}
#endif

static int
read_sidechannel(char *devid, size_t size, double timeout)
{
    int			len = (int) size - 1;
    cups_sc_status_t	status;

    status = cupsSideChannelDoRequest(CUPS_SC_CMD_GET_DEVICE_ID,
	devid, &len, timeout);
    if (status != CUPS_SC_STATUS_OK || len <= 0)
    {
	fprintf(stderr, "DEBUG: %s: no device ID from the backend (%d)\n",
	    Progname, (int) status);
	return -1;
    }
    devid[len] = '\0';
    return 0;
}

/*
 * CUPS 2.4 copies marker-names into printer-supply-description, but on the
 * next update it fails to find (and delete) the previous copy, so every
 * update adds a duplicate attribute.  Only send the names when the queue
 * doesn't have them yet, or lost printer-supply-description (it isn't
 * restored when cupsd restarts).
 */
static int
cups_has_names(void)
{
    const char		*printer = getenv("PRINTER");
    char		uri[1024];
    ipp_t		*request, *response;
    ipp_attribute_t	*attr;
    int			i, same = 0;
    static const char	*attrs[] =
    {
	"marker-names", "printer-supply-description"
    };

    if (!printer || !*printer)
	return 0;
    httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL,
	"localhost", ippPort(), "/printers/%s", printer);
    request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri",
	NULL, uri);
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
	"requested-attributes", 2, NULL, attrs);
    if (!(response = cupsDoRequest(CUPS_HTTP_DEFAULT, request, "/")))
	return 0;
    attr = ippFindAttribute(response, "marker-names", IPP_TAG_ZERO);
    if (attr && ippGetCount(attr) == NTONERS
	&& ippFindAttribute(response, "printer-supply-description",
	    IPP_TAG_ZERO))
    {
	same = 1;
	for (i = 0; i < NTONERS; ++i)
	    if (strcmp(ippGetString(attr, i, NULL), Toners[i].name))
		same = 0;
    }
    ippDelete(response);
    return same;
}

static void
print_cups(const Status *st)
{
    char	names[512] = "", colors[128] = "", types[128] = "",
		levels[64] = "", low[64] = "", high[64] = "";
    int		i, any_low = 0, any_empty = 0, any_missing = 0;

    for (i = 0; i < NTONERS; ++i)
    {
	const char	*sep = i ? "," : "";
	const TonerState *t = &st->toner[i];

	snprintf(names + strlen(names), sizeof(names) - strlen(names),
	    "%s\"%s\"", sep, Toners[i].name);
	snprintf(colors + strlen(colors), sizeof(colors) - strlen(colors),
	    "%s%s", sep, Toners[i].color);
	snprintf(types + strlen(types), sizeof(types) - strlen(types),
	    "%stoner", sep);
	snprintf(levels + strlen(levels), sizeof(levels) - strlen(levels),
	    "%s%d", sep, t->level);
	snprintf(low + strlen(low), sizeof(low) - strlen(low),
	    "%s%d", sep, LOW_LEVEL);
	snprintf(high + strlen(high), sizeof(high) - strlen(high),
	    "%s100", sep);
	any_low |= t->low;
	any_empty |= t->empty;
	any_missing |= t->missing;
    }

    fprintf(stderr, "ATTR: marker-colors=%s\n", colors);
    if (!cups_has_names())
	fprintf(stderr, "ATTR: marker-names=%s\n", names);
    fprintf(stderr, "ATTR: marker-types=%s\n", types);
    fprintf(stderr, "ATTR: marker-low-levels=%s\n", low);
    fprintf(stderr, "ATTR: marker-high-levels=%s\n", high);
    fprintf(stderr, "ATTR: marker-levels=%s\n", levels);
    fprintf(stderr, "STATE: %ctoner-low-warning\n", any_low ? '+' : '-');
    fprintf(stderr, "STATE: %ctoner-empty-error\n", any_empty ? '+' : '-');
    fprintf(stderr, "STATE: %cmarker-supply-missing-error\n",
	any_missing ? '+' : '-');
    /* Only reported once the printer tried to feed a sheet */
    fprintf(stderr, "STATE: %cmedia-empty-error\n", st->media_empty ? '+' : '-');
    fprintf(stderr, "STATE: %cmedia-needed\n", st->media_empty ? '+' : '-');
    fprintf(stderr, "STATE: %ccover-open-error\n", st->cover_open ? '+' : '-');
    if (has_error(st))
	fprintf(stderr, "DEBUG: %s: printer error %03d-%03d (%s)\n",
	    Progname, st->err_major, st->err_minor, error_text(st));
    fflush(stderr);
}

static void
print_json(const Status *st)
{
    int		i;
    const char	*p;

    printf("{\"model\":\"");
    for (p = st->model; *p; ++p)
	if (*p == '"' || *p == '\\')
	    printf("\\%c", *p);
	else if ((unsigned char) *p >= 0x20)
	    putchar(*p);
    printf("\",\"plausible\":%s,\"state\":%d,\"substate\":%d,",
	st->plausible ? "true" : "false", st->state, st->substate);
    if (has_error(st))
	printf("\"error\":\"%03d-%03d\",\"error_text\":\"%s\",",
	    st->err_major, st->err_minor, error_text(st));
    else
	printf("\"error\":null,\"error_text\":null,");
    printf("\"media_empty\":%s,\"cover_open\":%s,\"low_level\":%d,"
	"\"toners\":[", st->media_empty ? "true" : "false",
	st->cover_open ? "true" : "false", LOW_LEVEL);
    for (i = 0; i < NTONERS; ++i)
    {
	const TonerState *t = &st->toner[i];

	printf("%s{\"name\":\"%s\",\"color\":\"%s\",\"level\":%d,"
	    "\"missing\":%s,\"low\":%s,\"empty\":%s}", i ? "," : "",
	    Toners[i].name, Toners[i].color, t->level,
	    t->missing ? "true" : "false", t->low ? "true" : "false",
	    t->empty ? "true" : "false");
    }
    printf("],\"sts\":\"");
    for (i = 0; i < st->len; ++i)
	printf("%02x", st->sts[i]);
    printf("\"}\n");
}

static void
print_text(const Status *st)
{
    int		i;

    printf("Model:  %s\n", st->model[0] ? st->model : "(unknown)");
    printf("State:  0x%02x 0x%02x\n", st->state, st->substate);
    if (has_error(st))
	printf("Error:  %03d-%03d (%s)\n",
	    st->err_major, st->err_minor, error_text(st));
    else
	printf("Error:  none\n");
    for (i = 0; i < NTONERS; ++i)
    {
	const TonerState *t = &st->toner[i];

	if (t->missing)
	    printf("%-14s missing\n", Toners[i].name);
	else if (t->level < 0)
	    printf("%-14s unknown\n", Toners[i].name);
	else
	    printf("%-14s %3d%%%s\n", Toners[i].name, t->level,
		t->empty ? " (empty)" : t->low ? " (low)" : "");
    }
    if (!st->plausible)
	printf("Warning: unknown STS layout, the levels above may be wrong\n");
    printf("STS:   ");
    for (i = 0; i < st->len; ++i)
	printf(" %02x", st->sts[i]);
    printf("\n");
}

static int
report(const char *devid, int out)
{
    Status	st;

    if (decode(devid, &st) < 0)
	return 1;
    if (out == OUT_CUPS && !st.plausible)
    {
	fprintf(stderr, "DEBUG: %s: unknown STS layout, not reporting\n",
	    Progname);
	return 1;
    }
    if (out == OUT_CUPS)
	print_cups(&st);
    else if (out == OUT_JSON)
	print_json(&st);
    else
	print_text(&st);
    return 0;
}

/*
 * CUPS command filter: handle "ReportLevels" from a command file.
 * Nothing is written to stdout, so nothing is sent to the printer.
 */
static int
command_filter(int argc, char *argv[])
{
    FILE	*fp = stdin;
    char	line[1024], *p, devid[DEVID_MAX];

    if (argc == 7 && (fp = fopen(argv[6], "r")) == NULL)
    {
	perror("ERROR: Unable to open command file");
	return 1;
    }
    while (fgets(line, sizeof(line), fp))
    {
	for (p = line; isspace((unsigned char) *p); ++p)
	    ;
	p[strcspn(p, "\r\n")] = '\0';
	if (*p == '#' || !*p)
	    continue;
	if (strncasecmp(p, "ReportLevels", 12) == 0)
	{
	    if (read_sidechannel(devid, sizeof(devid), SC_TIMEOUT_FILTER) == 0)
		report(devid, OUT_CUPS);
	}
	else
	    fprintf(stderr, "DEBUG: %s: unsupported command \"%s\"\n",
		Progname, p);
    }
    if (fp != stdin)
	fclose(fp);
    return 0;
}

int
main(int argc, char *argv[])
{
    int		c, out = OUT_TEXT, sidechannel = 0;
    char	*devid_arg = NULL, devid[DEVID_MAX];

    /*
     * CUPS runs filters as "printer job user title copies options [file]",
     * with argv[0] set to the queue name, so look at the arguments.
     */
    if ((argc == 6 || argc == 7) && argv[1][0]
	&& strspn(argv[1], "0123456789") == strlen(argv[1]))
	return command_filter(argc, argv);

    while ((c = getopt(argc, argv, "cji:sh?")) != EOF)
	switch (c)
	{
	case 'c':	out = OUT_CUPS; break;
	case 'j':	out = OUT_JSON; break;
	case 'i':	devid_arg = optarg; break;
	case 's':	sidechannel = 1; break;
	default:	usage(); break;
	}
    argc -= optind;
    argv += optind;
    if (argc > 1 || (argc == 1 && (devid_arg || sidechannel)))
	usage();

    if (devid_arg)
	return report(devid_arg, out);
    if (sidechannel)
    {
	/* Called from a print job: never fail the job */
	if (read_sidechannel(devid, sizeof(devid), SC_TIMEOUT_WRAPPER) == 0)
	    report(devid, out);
	return 0;
    }
#ifdef __linux__
    if (read_usblp(argc ? argv[0] : "/dev/usb/lp0", devid, sizeof(devid)) < 0)
	return 2;
    return report(devid, out);
#else
    fprintf(stderr, "%s: reading a device is only supported on Linux; "
	"use -s or -i\n", Progname);
    return 2;
#endif
}
