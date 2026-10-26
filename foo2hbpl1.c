/*

GENERAL
This program converts bilevel PBM, 8-bit PGM, 24-bit PPM, and 32-bit
CMYK PAM files (output by Ghostscript as "pbmraw", "pgmraw", "ppmraw",
and "pamcmyk32" respectively) to HBPL version 1 for the consumption
of various Dell, Epson, and Fuji-Xerox printers.

With this utility, you can print to some Dell and Fuji printers, such as these:
    - Generic HBPL1 Printer		B/W and Color
    - Dell 1250c			B/W and Color
    - Dell C1660			B/W and Color
    - Dell C1760			B/W and Color
    - Epson AcuLaser C1700		B/W and Color
    - Fuji-Xerox DocuPrint CP105	B/W and Color
These -z1 printers appear to recognize an extended command set:
    - Generic HBPL1 z1 Printer		B/W and Color	-z1
    - Xerox Phaser 6000B		B/W and Color	-z1
    - Xerox Phaser 6010N		B/W and Color	-z1
Printers listed above are personal type with 1 autotray (maybe plus a manual feed)
xml and PPD files that use this driver are located in package foomatic-db

AUTHORS
This program began life as Robert Szalai's 'pbmtozjs' program,
and then overhauled by Rick Richardson with several improvements.
foo2hbpl1.c originally began life as foo2hbpl2.c and was modified
for use with HBPLv1 type printers by Dave Coffin in March 2014.
Several improvements added later in August 2026 by Joe Da Silva.

LICENSE
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or (at
your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see
<https://www.gnu.org/licenses/>.

*/

static const char Version[] = "$Id: foo2hbpl1.c,v 1.4 2026/08/23 12:00:00 joe Exp $";

#define _FILE_OFFSET_BITS 64
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#ifdef linux
    #include <sys/utsname.h>
#endif

#define LOGICAL_CLIP_X	2
#define LOGICAL_CLIP_Y	1

typedef struct
{
    unsigned char *buf;
    int size, off, bits;
} Stream;

typedef struct
{
    int    AllIsBlack;
    int    BlackClears;
    int    Color2Mono;		// default=0=off
    int    Copies;		// [1..999] Page Copies (default=1)
    int    Debug;		// Debug>=9 if md5sum testpage.ps results
    int    MediaCode;		// -1=undefined (default to paper)
    int    PaperCode;		// (default=letter)
    int    LogicalClip;		// default=0=no, 1=Y, 2=X, 3=XY
    int    Model;		// -1=undefined (default -z0)
    int    pagenum;		// no pages, no printer codes sent
    int    SaveToner;		// -t, every second pixel is blank
    int    PrintSize;		// printer job byte count, <= 50M
    int    PrintMax;		// default=off limit JOB size
    int    PrintReset;		// RESET printer before/after JOB
    int    PageWidth;
    int    PageHeight;
    int    Width;
    int    Height;
    int    UpperLeftX;		// clipping margins [left,top,right,bottom]
    int    UpperLeftY;
    int    LowerRightX;
    int    LowerRightY;
    Stream stream[5];
    unsigned char *image;	// (height+2)x(width+4)x(deep,1=GRAY,4=YCMK)
    char  *Username;
    char  *Filename;
} Job;

static const char *mname[2+24] = { //Known media types
	"COATEDPAPER2",		// z1/--, 4=coated, light weight glossy card? (z1)
	"RECYCLE",		// z1/--, 7=recycled paper
	// maintain -m[1..12] media compatibility sequence for -z0,-z1,-zX...
	"NORMAL",		// z1/z0, 1=plain paper
	"THICK",		// z1/z0, 2=thick, bond paper
	"HIGHQUALITY",		// z1/z0, 3=high-quality, premium, cotton?
	"COAT2",		// --/z0, 4=coated, light weight glossy card? (z0)
	"LABEL",		// z1/z0, 5=label
	"ENVELOPE",		// z1/z0, 6=envelope
	"RECYCLED",		// --/z0, 7=recycled paper
	"NORMALREV",		// z1/z0, 8=plain paper (side2)
	"THICKSIDE2",		// z1/z0, 9=thick, bond paper (side2)
	"HIGHQUALITYREV",	// z1/z0, 10=high-quality, premium, cotton? (side2)
	"COATEDPAPER2REV",	// z1/z0, 11=coated, light weight glossy card? (side2)
	"RECYCLEREV",		// z1/z0, 12=recycled paper (side2)
	// include these -m[1..24] media codes for -z1
	"LETTERHEAD",		// z1/--, 13,letterhead
	"LETTERHEADREV",	// z1/--, 14,letterhead (side2)
	"PREPRINTED",		// z1/--, 15,pre-printed
	"PREPRINTEDREV",	// z1/--, 16,pre-printed (side2)
	"PREPUNCHED",		// z1/--, 17,pre-punched?
	"PREPUNCHEDREV",	// z1/--, 18,pre-punched? (side2)
	"COLOR",		// z1/--, 19,colored
	"COLORREV",		// z1/--, 20,colored (side2)
	"USER1",		// z1/--, 21,custom (not sure if more params needed?)
	"USER1REV",		// z1/--, 22,custom (side2)
	"SPECIAL",		// z1/--, 23,special
	"SPECIALREV"		// z1/--, 24,special (side2)
};

static const char *pname[12] = { //Known paper types
	"LETTER",		// 0
	"LEGAL",		// 1
	"A4",			// 2
	"EXECUTIVE",		// 3
// unknown 4,5
	"COM10",		// 6
	"MONARCH",		// 7
	"C5",			// 8
	"DL",			// 9
// unknown 10
	"JISB5",		// 11
// unknown 12,13,14
	"A5",			// 15
	"FOLIO",		// 205
// fanfold german legal
	"CUSTOM"		// 255
};

static const short papers[] = { // Official sizes to nearest 1/600 inch
	// Official sizes to nearest 1/600 inch
	// will accept +-1.5mm (35/600 inch) tolerance
	// NOTE: printer appears only able to accept < 22cm max paper width
	// {codeHBPL, media=[envelope=6,default=1], sizeX, sizeY}
	  0, 5100, 6600, //0,l, 8.50" x 11.0" / 215.9mm x 279.4mm Letter
	  2, 5100, 8400, //1,l, 8.50" x 14.0" / 215.9mm x 355.6mm Legal
	  4, 4961, 7016, //2,l, 210.0mm x 297.0mm / 8.27" x 11.7" A4
	  6, 4350, 6300, //3,l, 7.25" x 10.5" / 184.2mm x 266.7mm Executive
// unknown 4
// unknown 5
	 13, 2475, 5700, //6,e, 4.125" x 9.5" / 104.8mm x 241.3mm #10 envelope
	 15, 2325, 4500, //7,e, 3.875" x 7.5" / 98.4mm x 190.5mm Monarch envelope
	 17, 3827, 5409, //8,e, 162.0mm x 229.0mm / 6.38" x 9.02" C5 envelope
	 19, 2599, 5197, //9,e, 110.0mm x 220.0mm / 4.33" x 8.67" DL envelope
// unknown 10
	 22, 4299, 6071, //11,l, 182.0mm x 257.0mm / 7.17" x 10.1" B5jis
// unknown 12
// unknown 13
// unknown 14
	 30, 3496, 4961, //15,l, 148.0mm x 210.0mm / 5.83" x 8.27" A5
	410, 5100, 7800, //205,l, 8.50" x 13.0" / 215.9mm x 330.2mm Folio
//	205, 5100, 7800, //205,l, 8.5" x 13.0" / 215.9mm x 330.2mm fanfold german legal
	510		 //255,l, Custom paper size (needs also X and Y)
};

void
usage(Job *job)
{
    fprintf(stderr,
"Usage:\n"
"   foo2hbpl1 [options] <pamcmyk32-file >hbpl-file\n"
"\n"
"	Convert Ghostscript pbmraw, pgmraw, ppmraw, or pamcmyk32\n"
"	format to HBPLv1, for the Dell C1660w and other printers.\n"
"\n"
"	gs -q -dBATCH -dSAFER -dQUIET -dNOPAUSE \\ \n"
"		-sPAPERSIZE=letter -r600x600 -sDEVICE=pamcmyk32 \\ \n"
"		-sOutputFile=- - < testpage.ps \\ \n"
"	| foo2hbpl1 -m1 -z0 >testpage.zc\n"
"\n"
"Normal Options:\n"
"-m media	Media code to send to printer\n"
"		-z0:\n"
"		  1=plain, 2=thick, 3=high-quality, 4=coated,\n"
"		  5=label, 6=envelope, 7=recycled, 8=plain (side2),\n"
"		  9=thick (side2), 10=high-quality (side2),\n"
"		  11=coated (side2), 12=recycled (side2)\n"
"		-z1: above plus\n"
"		  13=letterhead, 14=letterhead (side2),\n"
"		  15=preprinted, 16=preprinted (side2),\n"
"		  17=prepunched, 18=prepunched (side2),\n"
"		  19=color, 20=color (side2),\n"
"		  21=user, 22=user (side2),\n"
"		  23=special, 24=special (side2)\n"
"-p paper	Paper code autodetected by printer [%d]\n"
"		  0=Letter, 1=Legal, 2=A4, 3=Executive, 6=Env10,\n"
"		  7=EnvMonarch, 8=EnvC5, 9=EnvDL, 11=B5jis,\n"
"		  15=A5, 205=Folio, 255=Custom (XxY)\n"
"-n copies	Number of copies [%d]\n"
"-t		Draft mode. Every other pixel is white.\n"
"-J filename	Filename string to send to printer [%s]\n"
"-U username	Username string to send to printer [%s]\n"
"\n"
"Printer Tweaking Options:\n"
"-u <xoff>x<yoff> Set upper-left clip margin offset [%dx%d] pixels\n"
"-l <xoff>x<yoff> Set lower-right clip margin offset [%dx%d] pixels\n"
"-L mask	Send logical clipping values from -u/-l [%d]\n"
"		  0=no, 1=Y, 2=X, 3=XY\n"
"-e		no RESET printer before or after JOB (exlude -z0)\n"
"-f size	Break JOB into one or more smaller JOBs [%d]\n"
"		  0=Off, 1=~5M, 2=~20M, 3=~100M\n"
"-A		AllIsBlack: convert C=1,M=1,Y=1 to just K=1\n"
"-B		BlackClears: K=1 forces C,M,Y to 0\n"
"-z model	Model: [%d]\n"
"                 0=(default) (Need more info for list)\n"
"                 1=(example: Xerox 6000/6010)\n"
"\n"
"Debugging Options:\n"
"-S plane	Output a single color plane from a color print [%d]\n"
"		and print it on the black plane. Default all colors\n"
"		  0=off, 1=Cyan, 2=Magenta, 3=Yellow, 4=Black\n"
"-D lvl		Set Debug level [%d]\n"
"-V		Version %s\n"
	, job->PaperCode
	, job->Copies
	, job->Filename ? job->Filename : ""
	, job->Username ? job->Username : ""
	, job->UpperLeftX, job->UpperLeftY
	, job->LowerRightX, job->LowerRightY
	, job->LogicalClip
	, job->PrintMax
	, job->Model
	, job->Color2Mono
	, job->Debug
	, Version);
}

void
debug(Job *job, int level, char *fmt, ...)
{
    va_list ap;

    if (job->Debug < level)
	return;

    setvbuf(stderr, (char *) NULL, _IOLBF, BUFSIZ);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void
reset_streams(Job *job)
{
    int i;

    for (i = 0; i < 5; ++i)
    {
	if (job->stream[i].buf)
	    free(job->stream[i].buf);
	job->stream[i].buf = NULL;
	job->stream[i].size = job->stream[i].off = job->stream[i].bits = 0;
    }
}

void
end_doc(Job *job, int done)
{
    printf("\033%%-12345X@PJL EOJ\n");
    if (job->Model > 0 && job->PrintReset > 0 && done > 0)
	printf("@PJL RESET\n");
    debug(job, 1, "End printer JOB.\n");
}

void
error(Job *job, int fatal, char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    if (fatal) {
	if (job->pagenum)
	    end_doc(job, 1);
	reset_streams(job);
	if (job->image)
	    free(job->image);
	exit(fatal);
    }
}

void
save_toner(Job *job, int color)
{
    unsigned char *dp;
    unsigned int i, row, col;

    color = (color ? 4 : 1);

    // checker pattern 0xAA/0x55, 8bpp*color
    for (row = 0; row < job->Height; row += 2)
    {
	dp = job->image + row * job->Width * color;
	for (col = 0; col < job->Width; col += 2)
	{
	    for (i = 0; i < color; ++i)
		*dp++ = 0;
	    dp += color;
	}
    }
    for (row = 1; row < job->Height; row += 2)
    {
	dp = job->image + row * job->Width * color;
	for (col = 1; col < job->Width; col += 2)
	{
	    dp += color;
	    for (i = 0; i < color; ++i)
		*dp++ = 0;
	}
    }
    debug(job, 2, "  Done save_toner(%s)\n", (color == 4 ? "Color" : "Gray"));
}

void
all_is_black(Job *job)
{
    unsigned char *p;
    unsigned int row, col;

    for (row = 0; row < job->Height; ++row)
    {
	p = job->image + row * job->Width * 4; // KCMY
	for (col = 0; col < job->Width; ++col)
	{
	    if (p[1] == 255 && p[2] == 255 && p[3] == 255)
	    {
		*p++ = 255; *p++ = 0; *p++ = 0; *p++ = 0;
		//fprintf(stderr,"a");
	    }
	    else
		p += 4;
	}
    }
    debug(job, 2, "  Done all_is_black()\n");
}

void
black_clears(Job *job)
{
    unsigned char *p;
    unsigned int row, col;

    for (row = 0; row < job->Height; ++row)
    {
	p = job->image + row * job->Width * 4; // KCMY
	for (col = 0; col < job->Width; ++col)
	{
	    if (*p++ == 255)
	    {
		*p++ = 0; *p++ = 0; *p++ = 0;
		//fprintf(stderr,"b");
	    }
	    else
		p += 3;
	}
    }
    debug(job, 2, "  Done black_clears()\n");
}

void
color_to_gray(Job *job, int *deep)
{
    unsigned char *dp, *sp;
    unsigned int row, col;

    for (row = 0; row < job->Height; ++row)
    {
	sp = (job->image + row * job->Width * 4 + (job->Color2Mono & 3)); // KCMY
	dp = (job->image + row * job->Width);
	for (col = 0; col < job->Width; ++col)
	{
	    *dp++ = *sp;
	    sp += 4;
	}
    }
    *deep = 1;
    debug(job, 2, "  Done color_to_gray()\n");
}

#define MAXSTREAM (8.5*14*2400*1200)-0x100000
void
putbits(Job *job, int n, unsigned val, int nbits)
{
    if (job->stream[n].off + 16 > job->stream[n].size && \
        (job->stream[n].size > MAXSTREAM || \
	!(job->stream[n].buf = realloc(job->stream[n].buf, job->stream[n].size += 0x100000))))
	    error(job, 1, "Out of memory\n");
    if (job->stream[n].bits)
    {
	job->stream[n].off--;
	val |= job->stream[n].buf[job->stream[n].off] >> (8-job->stream[n].bits) << nbits;
	nbits += job->stream[n].bits;
    }
    job->stream[n].bits = nbits & 7;
    while ((nbits -= 8) > 0)
	job->stream[n].buf[job->stream[n].off++] = val >> nbits;
    job->stream[n].buf[job->stream[n].off++] = val << -nbits;
}
#undef MAXSTREAM

/*
   Runlengths are integers between 1 and 17057 encoded as follows:

	1	00
	2	01 0
	3	01 1
	4	100 0
	5	100 1
	6	101 00
	7	101 01
	8	101 10
	9	101 11
	10	110 0000
	11	110 0001
	12	110 0010
	   ...
	25	110 1111
	26	111 000 000
	27	111 000 001
	28	111 000 010
	29	111 000 011
	   ...
	33	111 000 111
	34	111 001 000
	   ...
	41	111 001 111
	42	111 010 000
	50	111 011 0000
	66	111 100 00000
	98	111 101 000000
	162	111 110 000000000
	674	111 111 00000000000000
	17057	111 111 11111111111111
*/
void
put_len(Job *job, int n, unsigned val)
{
    static const unsigned code[34] =
    {
	  1, 0, 2,
	  2, 2, 3,
	  4, 8, 4,
	  6, 0x14, 5,
	 10, 0x60, 7,
	 26, 0x1c0, 9,
	 50, 0x3b0, 10,
	 66, 0x780, 11,
	 98, 0xf40, 12,
	162, 0x7c00, 15,
	674, 0xfc000, 20,
	17058
    };
    int c = 0;

    if (val < 1 || val > 17057) return;
    while (val >= code[c+3]) c += 3;
    putbits(job, n, val-code[c] + code[c+1], code[c+2]);
}

/*
   CMYK byte differences are encoded as follows:

	 0	000
	+1	001
	-1	010
	 2	011s0	s = 0 for +, 1 for -
	 3	011s1
	 4	100s00
	 5	100s01
	 6	100s10
	 7	100s11
	 8	101s000
	 9	101s001
	    ...
	 14	101s110
	 15	101s111
	 16	110s00000
	 17	110s00001
	 18	110s00010
	    ...
	 46	110s11110
	 47	110s11111
	 48	1110s00000
	 49	1110s00001
	    ...
	 78	1110s11110
	 79	1110s11111
	 80	1111s000000
	 81	1111s000001
	    ...
	 126	1111s101110
	 127	1111s101111
	 128	11111110000
*/
void
put_diff(Job *job, int n, signed char val)
{
    static const unsigned short code[25] =
    {
	 2,  3, 3, 1,
	 4,  4, 3, 2,
	 8,  5, 3, 3,
	16,  6, 3, 5,
	48, 14, 4, 5,
	80, 15, 4, 6,
	129
    };
    int sign, abs, c = 0;

    switch (val)
    {
    case  0:  putbits(job, n, 0, 3);  return;
    case  1:  putbits(job, n, 1, 3);  return;
    case -1:  putbits(job, n, 2, 3);  return;
    }
    abs = ((sign = val < 0)) ? -val:val;
    while (abs >= code[c+4]) c += 4;
    putbits(job, n, code[c+1], code[c+2]);
    putbits(job, n, sign, 1);
    putbits(job, n, abs-code[c], code[c+3]);
}

void
setle(unsigned char *c, int s, int i)
{
    while (s--)
    {
	*c++ = i;
	i >>= 8;
    }
}

void
start_doc(Job *job, int done, int color)
{
    static const char reca[12] =
    {
	0x41,			// 0,RECTYPE 'A'
	0x81,0xa1,0x00,
	0x82,0xa2,0x07,0x00,
	0x83,0xa2,0x01,0x00
    };
    time_t t;
    struct tm *tmp;
    char datestr[16], timestr[16];
    char cname[128] = "My Computer";

    t = time(NULL);
    tmp = localtime(&t);
    strftime(datestr, sizeof datestr, "%m/%d/%Y", tmp);
    strftime(timestr, sizeof timestr, "%H:%M:%S", tmp);

    #ifdef linux
    {
	struct utsname u;

	uname(&u);
	strncpy(cname, u.nodename, 128);
	cname[127] = 0;
    }
    #endif

    /* Lines end with \n, not \r\n */

    printf(
	"\033%%-12345X%s@PJL SET STRINGCODESET=UTF8\n"
	"@PJL COMMENT DATE=%s\n"
	"@PJL COMMENT TIME=%s\n"
	"@PJL COMMENT DNAME=%s\n"
	"@PJL JOB MODE=PRINTER\n"
	"@PJL SET JOBATTR=\"@LUNA=%s\"\n"
	"@PJL SET JOBATTR=\"@TRCH=OFF\"\n"
	"@PJL SET DUPLEX=OFF\n"
	"@PJL SET BINDING=LONGEDGE\n"
	"@PJL SET IWAMANUALDUP=OFF\n"
	"@PJL SET JOBATTR=\"@MSIP=%s\"\n"
	"@PJL SET RENDERMODE=%s\n"
	"@PJL SET ECONOMODE=OFF\n"
	"@PJL SET RET=ON\n"
	"@PJL SET JOBATTR=\"@IREC=OFF\"\n"
	"@PJL SET JOBATTR=\"@TRAP=ON\"\n"
	"@PJL SET JOBATTR=\"@JOAU=%s\"\n"
	"@PJL SET JOBATTR=\"@CNAM=%s\"\n"
	"@PJL SET COPIES=%d\n"
	"@PJL SET QTY=1\n"
	"@PJL SET PAPERDIRECTION=SEF\n"
	"@PJL SET RESOLUTION=600\n"
	"@PJL SET BITSPERPIXEL=8\n"
	"@PJL SET JOBATTR=\"@DRDM=XRC\"\n"
	"@PJL SET JOBATTR=\"@TSCR=11\"\n"
	"@PJL SET JOBATTR=\"@GSCR=11\"\n"
	"@PJL SET JOBATTR=\"@ISCR=12\"\n"
	"@PJL SET JOBATTR=\"@TTRC=11\"\n"
	"@PJL SET JOBATTR=\"@GTRC=11\"\n"
	"@PJL SET JOBATTR=\"@ITRC=12\"\n"
	"@PJL SET JOBATTR=\"@TCPR=11\"\n"
	"@PJL SET JOBATTR=\"@GCPR=11\"\n"
	"@PJL SET JOBATTR=\"@ICPR=12\"\n"
	"@PJL SET JOBATTR=\"@TUCR=11\"\n"
	"@PJL SET JOBATTR=\"@GUCR=11\"\n"
	"@PJL SET JOBATTR=\"@IUCR=12\"\n"
	"@PJL SET JOBATTR=\"@BSPM=OFF\"\n"
	"@PJL SET JOBATTR=\"@TDFT=0\"\n"
	"@PJL SET JOBATTR=\"@GDFT=0\"\n"
	"@PJL SET JOBATTR=\"@IDFT=0\"\n"
	"@PJL ENTER LANGUAGE=HBPL\n"
	, ((job->Model > 0 && job->PrintReset > 0 && done > 0) ? "@PJL RESET\n" : "")
	, (job->Debug < 9 ? datestr : "02/26/2026")
	, (job->Debug < 9 ? timestr : "12:34:56")
	, job->Filename ? job->Filename : ""
	, job->Username ? job->Username : ""
	, mname[job->MediaCode]
	, color ? "COLOR" : "GRAYSCALE"
	, job->Username ? job->Username : ""
	, cname
	, job->Copies);
    fwrite (reca, 1, sizeof(reca), stdout);

    debug(job, 1, "  Done start_doc(%d). Init printer JOB.\n", color);
    job->pagenum++; // Now begin printing as "JOB MODE=PRINTER START=1"...
    job->PrintSize += 1096+12;
}

#define IP (((int *)job->image) + off)
#define CP (((char *)job->image) + off)
#define DP (((char *)job->image) + off*deep)
#define BP(x) ((blank[(off+x) >> 3] << ((off+x) & 7)) & 128)
#define put_token(j,n,x) putbits(job,n,huff[hsel][x] >> 4, huff[hsel][x] & 15)

void
encode_page(Job *job, int color, int width, int height)
{
    unsigned char head[90] =
    {
	0x43,			// 0,RECTYPE 'C'
	0x91,0xa1,0x00,
	0x92,0xa1,0x01,
	0x93,0xa1,0x01,
	0x94,0xa1,
	0x00,			// 12,paper
	0x95,0xc2,
	0x00,0x00,		// 15,width,custom paper size
	0x00,0x00,		// 17,height,custom paper size
	0x96,0xa1,
	0x00,			// 21,custom paper size (insert 2)
	0x97,0xc3,0x00,0x00,0x00,0x00,0x98,0xa1,0x00,0x99,0xa4,
	0x01,0x00,0x00,0x00,	// 33,pagenum
	0x9a,0xc4,
	0x00,0x00,0x00,0x00,	// 39,width
	0x00,0x00,0x00,0x00,	// 43,height
	0x9b,0xa1,0x00,0x9c,0xa1,0x01,0x9d,0xa1,
	0x00,			// 55,grayscale=0x00001001 or color=0x10001011
	0x9e,0xa1,0x02,0x9f,0xa1,0x05,0xa0,0xa1,0x08,0xa1,0xa1,0x00,0xa2,0xc4,
	0x00,0x00,0x00,0x00,	// 70,width
	0x00,0x00,0x00,0x00,	// 74,height
	0x51,0x52,0xa3,0xa1,0x00,0xa4,0xb1,
	0xa4,			// 85,(total<=0xffff)=a2,(total>0xffff)=a4
	0x00,0x00, 0x00,0x00	// 86,total
    };
    unsigned char body[52] =
    {
	0x20,0x00,0x00,0x00,
	0x00,			// 4,if grayscale then set this to 8
	0x01,
	0x00,			// 6,if color then set this to 1
	0x00,0x10,0x32,0x04,0x00,0xa1,0x42,0x00,0x00,0x00,0x00,0xff,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,
	0x00,0x00,0x00,0x00,	// 32,deep0,only this if grayscale
	0x00,0x00,0x00,0x00,	// 36,deep1
	0x00,0x00,0x00,0x00,	// 40,deep2
	0x00,0x00,0x00,0x00,	// 44,deep3
	0x00,0x00,0x00,0x00
    };
    static const unsigned short huff[2][8] =
    {
	{ 0x01,0x63,0x1c5,0x1d5,0x1e5,0x22,0x3e6 }, // for text & graphics
	{ 0x22,0x63,0x1c5,0x1d5,0x1e5,0x01,0x3e6 }, // for images
    };
    unsigned char *blank, *blank0;
    int dirs[] = { -1,0,-1,1,2 }, rotor[] = { 0,1,2,3,4 };
    int i, j, row, col, deep, dir, run, try, bdir, brun, total;
    int paper = 510, hsel = 0, off = 0, bit = 0, stat = 0;
    int margin = width-96;

    reset_streams(job);

    debug(job, 1, "  Start encode_page(%d)\n", job->pagenum);

    deep = 1 + color*3;

    // autodetect paper size (portrait)
    for (i = 0; papers[i] < 255*2; i+=3)
	if (abs(width-papers[i+1]) < 36 && abs(height-papers[i+2]) < 36)
	{
	    paper = i;
	    goto psize;
	}
    paper = i; // 255, use custom paper size if you are here
    setle(head+15, 2,  (width*254+300)/600);  // units of 0.1mm
    setle(head+17, 2, (height*254+300)/600);
    head[21] = 2;
psize:
    head[12] = papers[paper]>>1;
    if (job->MediaCode < 0)
	job->MediaCode = ((papers[paper] & 1) ? 6+1 : 1+1);

    debug(job, 2, "  width=%d height=%d color=%d deep=%d=%s\n", \
		     width, height, color, deep, color ? "CYMK" : "GRAY");
    debug(job, 2, "  paper#%d,%d=%s media=%d=%s\n", \
		     paper, papers[paper]>>1, pname[paper], \
		     (job->MediaCode >= 1+1 ? job->MediaCode-1 : job->MediaCode), \
		     mname[job->MediaCode]);

    if (!job->pagenum)
	start_doc(job, 1, color);
    else if (job->PrintMax > 0 && job->PrintSize > job->PrintMax)
    {
	debug(job, 1, "  Break JOB into smaller segments\n");
	end_doc(job, 0);
	job->pagenum = job->PrintSize = 0;
	start_doc(job, 0, color);
    }

    width = -(-width & -8);
    setle(head+33, 4, job->pagenum);
    setle(head+39, 4, width);
    setle(head+43, 4, height);
    setle(head+70, 4, width);
    setle(head+74, 4, height);
    head[55] = 9 + color*130;
    if (color)	body[6] = 1;
    else	body[4] = 8;

    for (i=1; i < 5; i++)
	dirs[i] -= width;
    if (!color) dirs[4] = -8;

    blank = blank0 = calloc(height+2, width/8);
    memset(blank++, -color, width/8+1);
    for (row = 1; row <= height; row++)
    {
	memset(job->image + row*width * deep + deep, -1, deep);
	for (col = 8; col < width*deep; col += 4)
	    if (*(int *)(job->image + row*width*deep + col))
	    {
		for (col = 12; col < margin/8; col++)
		    blank[row*(width/8)+col] = -1;
		blank[row*(width/8)+col] = -2 << (~margin & 7);
		break;
	    }
    }
    memset(job->image, -color, (width+1)*deep);
    job->image += (width+1)*deep;
    blank += width/8;

    while (off < width * height)
    {
	for (bdir = brun = dir = 0; dir < 5; dir++)
	{
	    try = dirs[rotor[dir]];
	    for (run = 0; run < 17057; run++, try++)
	    {
		if (color)
		{
		    if (IP[run] != IP[try]) break;
		}
		else
		    if (CP[run] != CP[try]) break;

		if (BP(run) != BP(try)) break;
	    }
	    if (run > brun)
	    {
		bdir = dir;
		brun = run;
	    }
	}
	if (brun == 0)
	{
	    put_token(job, 0, 5);
	    for (i = 0; i < deep; i++)
		put_diff(job, 1+i, DP[i] - DP[i-deep]);
	    bit = 0;
	    off++;
	    stat--;
	    continue;
	}
	if (brun > width * height - off)
	    brun = width * height - off;
	if (bdir)
	{
	    j = rotor[bdir];
	    for (i = bdir; i; i--)
		rotor[i] = rotor[i-1];
	    rotor[0] = j;
	}
	if ((off-1+brun)/width != (off-1)/width)
	{
	    if (abs(stat) > 8 && ((stat >> 31) & 1) != hsel)
	    {
		hsel ^= 1;
		put_token(job, 0, 6);
	    }
	    stat = 0;
	}
	stat += bdir == bit;
	put_token(job, 0, bdir - bit);
	put_len(job, 0, brun);
	bit = brun < 17057;
	off += brun;
    }

    putbits(job, 0, 0xff, 8);
    for (total = 48, i = 0; i <= deep; i++)
    {
	putbits(job, i, 0xff, 8);
	job->stream[i].off--;
	setle(body+32 + i*4, 4, job->stream[i].off);
	total += job->stream[i].off;
    }
    head[85] = 0xa2 + (total > 0xffff)*2;
    setle(head+86, 4, total);
    fwrite(head, 1, 88+(total > 0xffff)*2, stdout);
    fwrite(body, 1, 48, stdout);
    job->PrintSize += 88+48;
    for (i = 0; i <= deep; i++)
    {
	fwrite(job->stream[i].buf, 1, job->stream[i].off, stdout);
	job->PrintSize += job->stream[i].off;
    }
    reset_streams(job);
    free(blank0);
    printf("SD");
    debug(job, 1, "  End encode_page(%d), Copies(%d). Print size ~ %d\n", \
		   job->pagenum, job->Copies, job->PrintSize);
    job->pagenum +=job->Copies;
}
#undef IP
#undef CP
#undef DP
#undef BP
#undef put_token

int
getint(Job *job, FILE *fp)
{
    int c, ret;

    for (;;)
    {
	while (isspace(c = fgetc(fp)));
	if (c == '#')
	{
	    while ((c = fgetc(fp)) != '\n')
		if (c < 0) return -1;
	}
	else
	    break;
    }
    if (!isdigit(c)) return -1;
    for (ret = c-'0'; isdigit(c = fgetc(fp)); )
	ret = ret*10 + c-'0';
    if (c < 0) return -1;
    debug(job, 3, "  getint(%d)\n", ret);
    return ret;
}

void
do_file(Job *job, FILE *fp)
{
    int type, iwide, ihigh, ideep, imax, ibyte;
    int wide, deep, byte, row, col, i, k;
    int lcl, lct, lcr, lcb;
    char tupl[128], line[128];
    unsigned char *image, *sp, *dp;

    debug(job, 1, "Start do_file()\n");

    lcl = lct = 0;
    if (job->LogicalClip & LOGICAL_CLIP_X)
	lcl = job->UpperLeftX;
    if (job->LogicalClip & LOGICAL_CLIP_Y)
	lct = job->UpperLeftY;

    while ((type = fgetc(fp)) != EOF)
    {
	type = ((type - 'P') << 8) | fgetc(fp);
	tupl[0] = iwide = ihigh = ideep = deep = imax = ibyte = -1;
	switch (type)
	{
	case '4':
	    deep = 1 + (ideep = 0);
	    goto six;
	case '5':
	    deep = ideep = 1;
	    goto six;
	case '6':
	    deep = 1 + (ideep = 3);
six:	    iwide = getint(job, fp);
	    ihigh = getint(job, fp);
	    imax = type == '4' ? 255 : getint(job, fp);
	    break;
	case '7':
	    do
	    {
		if (!fgets(line, sizeof(line), fp)) goto fail;
		if (!strncmp(line, "WIDTH ",6))
		    iwide = atoi(line + 6);
		if (!strncmp(line, "HEIGHT ",7))
		    ihigh = atoi(line + 7);
		if (!strncmp(line, "DEPTH ",6))
		    deep = ideep = atoi(line + 6);
		if (!strncmp(line, "MAXVAL ",7))
		    imax = atoi(line + 7);
		if (!strncmp(line, "TUPLTYPE ",9))
		    strncpy(tupl, line + 9, sizeof(line)-9-1);
	    } while (strcmp(line, "ENDHDR\n"));
	    if (ideep != 4 || strcmp(tupl, "CMYK\n")) goto fail;
	    break;
	default:
	    goto fail;
	}
	debug(job, 2, "  iwide=%d ihigh=%d imax=%d ideep=%d\n", \
			 iwide, ihigh, imax, ideep);
	if (iwide <= 0 || ihigh <= 0 || imax != 255) goto fail;
	wide = -(-iwide & -8);
        if (ideep)
	    ibyte = iwide * ideep;
	else
	    ibyte = wide >> 3;
	byte = wide * deep;

	debug(job, 2, "  wide=%d deep=%d ibyte=%d byte=%d\n", \
			 wide, deep, ibyte, byte);

	if (job->LogicalClip & LOGICAL_CLIP_X)
	    lcr = iwide - job->LowerRightX;
	else
	    lcr = iwide - job->LowerRightX - job->UpperLeftX;
	if (job->LogicalClip & LOGICAL_CLIP_Y)
	    lcb = ihigh - job->LowerRightY;
	else
	    lcb = ihigh - job->LowerRightY - job->UpperLeftY;
	if (lcr <= 0 || lcb <= 0 || \
	    job->UpperLeftY >= ihigh || job->LowerRightY >= ihigh || \
	    job->UpperLeftX >= iwide || job->LowerRightX >= iwide || \
	    job->UpperLeftY+job->LowerRightY >= ihigh || \
	    job->UpperLeftX+job->LowerRightX >= iwide)
	    lct = lcb = lcl = lcr = -1;

	debug(job, 2, "  Clip=[%d,%d,%d,%d] LC=%d lcl=%d lct=%d lcr=%d lcb=%d\n", \
			 job->UpperLeftX, job->UpperLeftY, \
			 job->LowerRightX, job->LowerRightY, \
			 job->LogicalClip, lcl, lct, lcr, lcb);

	job->Height = ihigh+2;
	job->Width = wide;
	job->image = image = (unsigned char *)(calloc(ihigh+2, byte));
	if (image == NULL)
	    error(job, 1, "Out of memory\n");
	for (row = 1; row <= ihigh; row++)
	{
	    i = fread(job->image, ibyte, 1, fp);
	    if (row > lct && row <= lcb)
	    {
		sp = job->image + lcl*ideep;
		dp = job->image + (row+job->UpperLeftY-lct)*byte + \
		     (2+job->UpperLeftX)*deep;
		//for (col = 0; col < iwide; col++)
		for (col = lcl; col < lcr; col++)
		{
		    switch (ideep)
		    {
		    case 0: // B&W 1bpp -> 8bpp
			*dp = ((job->image[col >> 3] >> (~col & 7)) & 1) * 255;
			break;
		    case 1: // GRAY 8bpp
			*dp = ~*sp;
			break;
		    case 3: // RGB -> KCMY
			for (k = sp[2], i = 0; i < 2; i++)
			    if (k < sp[i]) k = sp[i];
			*dp = ~k;
			for (i = 0; i < 3; i++)
			    dp[i+1] = k ? (k - sp[i]) * 255 / k : 0;
			break;
		    case 4: // CMYK -> KCMY
			for (i=0; i < 4; i++)
			    dp[i] = sp[((i-1) & 3)];
			break;
		    }
		    sp += ideep;
		    dp += deep;
		}
	    }
	}
	if (lct < 0)
	    memset(job->image, 0, (ihigh+2)*byte);
	else
	{
	    for (row = 1+job->UpperLeftY; row <= ihigh-job->LowerRightY; row++)
	    {
		memset(job->image + row*byte, 0, deep*(2+job->UpperLeftX));
		memset(job->image + row*byte + deep*(wide-2-job->LowerRightX), \
			0, deep*(2+job->LowerRightX));
	    }
	    memset(job->image, 0, byte*(1+job->UpperLeftY));
	    memset(job->image + byte*(1+ihigh-job->LowerRightY), 0, \
		   byte*(1+job->LowerRightY));
	}

	if (deep > 1) {
	    if (job->BlackClears)
		black_clears(job);
	    if (job->AllIsBlack)
		all_is_black(job);
	    if (job->Color2Mono)
		color_to_gray(job, &deep);
	}
	if (job->SaveToner)
	    save_toner(job, deep > 1);
	encode_page(job, deep > 1, iwide, ihigh);
	free(image);
	job->image = image = NULL;
    }
    debug(job, 1, "End do_file()\n");
    return;
fail:
    fprintf(stderr, "Not an acceptable PBM, PPM or PAM file!!!\n");
}

int
parse_xy(char *str, int *xp, int *yp)
{
    char *p;

    if (!str || str[0] == 0) return -1;

    *xp = strtoul(str, &p, 10);
    if (str == p) return -2;
    while (*p && (*p < '0' || *p > '9'))
	++p;
    str = p;
    if (str[0] == 0) return -3;
    *yp = strtoul(str, &p, 10);
    if (str == p) return -4;
    return (0);
}

int
main(int argc, char *argv[])
{
    Job job;
    int c, i;

    job.AllIsBlack = 0;
    job.BlackClears = 0;
    job.Color2Mono = 0;		// default=0=off
    job.Copies = 1;		// [1..999] Page Copies (default=1)
    job.Debug = 0;		// Debug>=9 if md5sum testpage.ps results
    job.MediaCode = -1;		// -1=undefined (default to paper)
    job.PaperCode = 0;		// (default=letter)
    job.LogicalClip = LOGICAL_CLIP_X | LOGICAL_CLIP_Y;
    job.Model = -1;		// -1=undefined (default -z0)
    job.pagenum = 0;		// no pages, no printer codes sent
    job.SaveToner = 0;
    job.PrintSize = 0;		// nothing sent to printer (so far)
    job.PrintMax = 0;		// default Off
    job.PrintReset = 1;		// default reset before/after print JOB
    job.PageWidth = 600 * 8.5;
    job.PageHeight = 600 * 11;
    job.UpperLeftX = 20;	// clip margin, left
    job.UpperLeftY = 20;	// clip margin, top
    job.LowerRightX = 20;	// clip margin, right
    job.LowerRightY = 20;	// clip margin, bottom
    job.image = NULL;
    job.Username = job.Filename = NULL;
    memset(job.stream, 0, sizeof(Stream)*5);

    while ((c = getopt(argc, argv, "f:l:m:n:p:u:z:D:J:L:S:U:etABV?h")) != EOF)
	switch (c)
	{
	case 'e': job.PrintReset = 0; break;
	case 'f': job.PrintMax = atoi(optarg); break;
	case 'm': job.MediaCode = atoi(optarg); break;
	case 'n': job.Copies = atoi(optarg); break;
	case 'p': job.PaperCode = atoi(optarg); break;
	case 't': job.SaveToner = 1; break;
	case 'u': if (strcmp(optarg, "0") == 0)
		      break;
		  if (parse_xy(optarg, &job.UpperLeftX, &job.UpperLeftY))
		      error(&job, 1, "Illegal format '%s' for -u\n", optarg);
		  break;
	case 'l': if (strcmp(optarg, "0") == 0)
		      break;
		  if (parse_xy(optarg, &job.LowerRightX, &job.LowerRightY))
		      error(&job, 1, "Illegal format '%s' for -l\n", optarg);
		  break;
	case 'A': job.AllIsBlack = -1; break;
	case 'B': job.BlackClears = -1; break;
	case 'J': if (optarg[0]) job.Filename = optarg; break;
	case 'U': if (optarg[0]) job.Username = optarg; break;
	case 'z': job.Model = atoi(optarg);
		  if (job.Model < 0 || job.Model > 1)
		      error(&job, 1, "Illegal value '%s' for -z\n", optarg);
		  break;
	case 'L': job.LogicalClip = atoi(optarg);
		  if (job.LogicalClip < 0 || job.LogicalClip > 3)
		      error(&job, 1, "Illegal value '%s' for -L\n", optarg);
		  break;
	case 'S': job.Color2Mono = atoi(optarg);
		  if (job.Color2Mono < 1 || job.Color2Mono > 4)
		      error(&job, 1, "Illegal value '%s' for -S\n", optarg);
	case 'D': job.Debug = atoi(optarg); break;
	case 'V': printf("%s\n", Version); return 0;
	default:  usage(&job); return 1;
	}

    if (job.Model < 0) job.Model = 0;
    if (job.MediaCode != -1 && (job.MediaCode <= 0 || ( \
	(job.Model == 0 && job.MediaCode > 12 ) || \
	(job.Model == 1 && job.MediaCode > 24))))
	error(&job, 1, "Illegal value for -m. For -z%d range is -m[1..%d]\n", \
	      job.Model, (job.Model ? 24 : 12), optarg);
    if (job.MediaCode != -1)
    {
	job.MediaCode++;
	if (job.Model > 0) {
	    if (job.MediaCode == 4+1) job.MediaCode = 0;
	    else if (job.MediaCode == 7+1) job.MediaCode = 1;
	}
    }
    if (job.Model <= 0 && job.Copies !=1)
	error(&job, 1, "Illegal value for -n. Must be 1 for -z0 printers!\n");
    if (job.Copies < 1 || job.Copies > 999)
	error(&job, 1, "Illegal value for -n%d. Must be a number [1..999]!\n", job.Copies);
    if (job.UpperLeftX < 20 || job.UpperLeftX >= job.PageWidth)
	error(&job, 1, "Illegal X value '%d' for -u\n", job.UpperLeftX);
    if (job.UpperLeftY < 20 || job.UpperLeftY >= job.PageHeight)
	error(&job, 1, "Illegal Y value '%d' for -u\n", job.UpperLeftY);
    if (job.LowerRightX < 20 || job.LowerRightX >= job.PageWidth)
	error(&job, 1, "Illegal X value '%d' for -l\n", job.LowerRightX);
    if (job.LowerRightY < 20 || job.LowerRightY >= job.PageHeight)
	error(&job, 1, "Illegal Y value '%d' for -l\n", job.LowerRightY);
    if (job.PrintMax == 0)
	job.PrintMax = -1; // don't break JOBs into smaller JOBs
    else if (job.PrintMax == 1)
	job.PrintMax = 5*1024*1024; // Break JOBs larger than 5MB
    else if (job.PrintMax == 2)
	job.PrintMax = 20*1024*1024; // Break JOBs larger than 20MB
    else if (job.PrintMax == 3)
	job.PrintMax = 100*1024*1024; // Break JOBs larger than 100MB
    else
	error(&job, 1, "Illegal value for -f%d. Must be [0..2]!\n", job.PrintMax);

    argc -= optind;
    argv += optind;

    if (argc == 0)
	do_file(&job, stdin);
    else
    {
	for (i = 0; i < argc; ++i)
	{
	    FILE *ifp;

	    if (!(ifp = fopen(argv[i], "r")))
		error(&job, 1, "Can't open '%s' for reading\n", argv[i]);
	    do_file(&job, ifp);
	    fclose(ifp);
	}
    }
    if (job.pagenum)
	end_doc(&job, 1);
    return 0;
}
