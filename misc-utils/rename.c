/*
 * rename.c - aeb 2000-01-01
 *
--------------------------------------------------------------
#!/bin/sh
if [ $# -le 2 ]; then echo call: rename from to files; exit; fi
FROM="$1"
TO="$2"
shift
shift
for i in $@; do N=`echo "$i" | sed "s/$FROM/$TO/g"`; mv "$i" "$N"; done
--------------------------------------------------------------
 * This shell script will do renames of files, but may fail
 * in cases involving special characters. Here a C version.
 */
#include <stdio.h>
#include <stdio_ext.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "nls.h"
#include "xalloc.h"
#include "c.h"
#include "closestream.h"
#include "rpmatch.h"

#define RENAME_EXIT_SOMEOK	2
#define RENAME_EXIT_NOTHING	4
#define RENAME_EXIT_UNEXPLAINED	64

static int tty_cbreak = 0;

static int string_replace(char *from, char *to, char *s, char *orig, char **newname)
{
	char *p, *q, *where;

	where = strstr(s, from);
	if (where == NULL)
		return 1;
	p = orig;
	*newname = xmalloc(strlen(orig) + strlen(to) + 1);
	q = *newname;
	while (p < where)
		*q++ = *p++;
	p = to;
	while (*p)
		*q++ = *p++;
	p = where + strlen(from);
	while (*p)
		*q++ = *p++;
	*q = 0;
	return 0;
}

static int ask(char *name)
{
	int c;
	char buf[2];
	printf(_("%s: overwrite `%s'? "), program_invocation_short_name, name);
	fflush(stdout);
	if ((c = fgetc(stdin)) == EOF) {
		buf[0] = 'n';
		clearerr(stdin); errno = 0;
		printf("n\n");
	}
	else {
		buf[0] = c;
		if (c != '\n' && tty_cbreak) {
			/* Possibly purge a multi-byte character; or do a
			   required purge of the rest of the line (including
			   the newline) if the tty has been put back in
			   canonical mode (for example by a shell after a
			   SIGTSTP signal). */
			__fpurge(stdin);
			printf("\n");
		}
		else if (c != '\n')
			while ((c = fgetc(stdin)) != '\n' && c != EOF);
	}
	buf[1] = '\0';
	if (rpmatch(buf) == RPMATCH_YES)
		return 0;
	else
		return 1;
}

static int do_symlink(char *from, char *to, char *s, int verbose, int noact,
                      int nooverwrite, int interactive)
{
	char *newname = NULL, *target = NULL;
	int ret = 1;
	struct stat sb;

	if ( faccessat(AT_FDCWD, s, F_OK, AT_SYMLINK_NOFOLLOW) != 0 &&
	     errno != EINVAL )
	   /* Skip if AT_SYMLINK_NOFOLLOW is not supported; lstat() below will
	      detect the access error */
	{
		warn(_("%s: not accessible"), s);
		return 2;
	}

	if (lstat(s, &sb) == -1) {
		warn(_("stat of %s failed"), s);
		return 2;
	}
	if (!S_ISLNK(sb.st_mode)) {
		warnx(_("%s: not a symbolic link"), s);
		return 2;
	}
	target = xmalloc(sb.st_size + 1);
	if (readlink(s, target, sb.st_size + 1) < 0) {
		warn(_("%s: readlink failed"), s);
		free(target);
		return 2;
	}
	target[sb.st_size] = '\0';
	if (string_replace(from, to, target, target, &newname))
		ret = 0;

	if (ret == 1 && (nooverwrite || interactive) && lstat(newname, &sb) != 0)
		nooverwrite = interactive = 0;

	if ( ret == 1 &&
	     (nooverwrite || (interactive && (noact || ask(newname) != 0))) )
	{
		if (verbose)
			printf(_("Skipping existing link: `%s' -> `%s'\n"), s, target);
		ret = 0;
	}

	if (ret == 1) {
		if (!noact && 0 > unlink(s)) {
			warn(_("%s: unlink failed"), s);
			ret = 2;
		} else if (!noact && symlink(newname, s) != 0) {
			warn(_("%s: symlinking to %s failed"), s, newname);
			ret = 2;
		}
	}
	if (verbose && (noact || ret == 1))
		if (verbose)
			printf("%s: `%s' -> `%s'\n", s, target, newname);
	free(newname);
	free(target);
	return ret;
}

static int do_file(char *from, char *to, char *s, int verbose, int noact,
                   int nooverwrite, int interactive)
{
	char *newname = NULL, *file=NULL;
	int ret = 1;

	if (access(s, F_OK) != 0) {
		warn(_("%s: not accessible"), s);
		return 2;
	}

	if (strchr(from, '/') == NULL && strchr(to, '/') == NULL)
		file = strrchr(s, '/');
	if (file == NULL)
		file = s;
	if (string_replace(from, to, file, s, &newname))
		return 0;

	if ((nooverwrite || interactive) && access(newname, F_OK) != 0)
		nooverwrite = interactive = 0;

	if (nooverwrite || (interactive && (noact || ask(newname) != 0))) {
		if (verbose)
			printf(_("Skipping existing file: `%s'\n"), newname);
		ret = 0;
	}
	else if (!noact && rename(s, newname) != 0) {
		warn(_("%s: rename to %s failed"), s, newname);
		ret = 2;
	}
	if (verbose && (noact || ret == 1))
		printf("`%s' -> `%s'\n", s, newname);
	free(newname);
	return ret;
}

static void __attribute__((__noreturn__)) usage(void)
{
	FILE *out = stdout;
	fputs(USAGE_HEADER, out);
	fprintf(out,
	      _(" %s [options] <expression> <replacement> <file>...\n"),
		program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Rename files.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -v, --verbose       explain what is being done\n"), out);
	fputs(_(" -s, --symlink       act on the target of symlinks\n"), out);
	fputs(_(" -n, --no-act        do not make any changes\n"), out);
	fputs(_(" -o, --no-overwrite  don't overwrite existing files\n"), out);
	fputs(_(" -i, --interactive   prompt before overwrite\n"), out);
	fputs(USAGE_SEPARATOR, out);
	printf(USAGE_HELP_OPTIONS(21));
	printf(USAGE_MAN_TAIL("rename(1)"));
	exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	char *from, *to;
	int i, c, ret = 0, verbose = 0, noact = 0, nooverwrite = 0, interactive = 0;
	struct termios tio;
	int (*do_rename)(char *from, char *to, char *s, int verbose, int noact,
	                 int nooverwrite, int interactive) = do_file;

	static const struct option longopts[] = {
		{"verbose", no_argument, NULL, 'v'},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{"no-act", no_argument, NULL, 'n'},
		{"no-overwrite", no_argument, NULL, 'o'},
		{"interactive", no_argument, NULL, 'i'},
		{"symlink", no_argument, NULL, 's'},
		{NULL, 0, NULL, 0}
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((c = getopt_long(argc, argv, "vsVhnoi", longopts, NULL)) != -1)
		switch (c) {
		case 'n':
			noact = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'o':
			nooverwrite = 1;
			interactive = 0;
			break;
		case 'i':
			interactive = 1;
			nooverwrite = 0;
			break;
		case 's':
			do_rename = do_symlink;
			break;
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage();
		default:
			errtryhelp(EXIT_FAILURE);
		}

	argc -= optind;
	argv += optind;

	if (argc < 3) {
		warnx(_("not enough arguments"));
		errtryhelp(EXIT_FAILURE);
	}

	from = argv[0];
	to = argv[1];

	if (!strcmp(from, to))
		return RENAME_EXIT_NOTHING;

	tty_cbreak = 0;
	if (interactive && isatty(STDIN_FILENO) != 0) {
		if (tcgetattr(STDIN_FILENO, &tio) != 0)
			warn(_("failed to get terminal attributes"));
		else if (!(tio.c_lflag & ICANON) && tio.c_cc[VMIN] == 1)
			tty_cbreak = 1;
	}

	for (i = 2; i < argc; i++)
		ret |= do_rename(from, to, argv[i], verbose, noact, nooverwrite, interactive);

	switch (ret) {
	case 0:
		return RENAME_EXIT_NOTHING;
	case 1:
		return EXIT_SUCCESS;
	case 2:
		return EXIT_FAILURE;
	case 3:
		return RENAME_EXIT_SOMEOK;
	default:
		return RENAME_EXIT_UNEXPLAINED;
	}
}
