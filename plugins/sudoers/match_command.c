/*
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 1996, 1998-2005, 2007-2023
 *	Todd C. Miller <Todd.Miller@sudo.ws>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

/*
 * This is an open source non-commercial project. Dear PVS-Studio, please check it.
 * PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
 */

#include <config.h>

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef SUDOERS_NAME_MATCH
# ifdef HAVE_GLOB
#  include <glob.h>
# else
#  include "compat/glob.h"
# endif /* HAVE_GLOB */
#endif /* SUDOERS_NAME_MATCH */
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#ifdef HAVE_FNMATCH
# include <fnmatch.h>
#else
# include "compat/fnmatch.h"
#endif /* HAVE_FNMATCH */
#include <regex.h>

#include "sudoers.h"
#include <gram.h>

#if !defined(O_EXEC) && defined(O_PATH)
# define O_EXEC O_PATH
#endif

static bool
regex_matches(const char *pattern, const char *str)
{
    const char *errstr;
    int errcode;
    regex_t re;
    debug_decl(regex_matches, SUDOERS_DEBUG_MATCH);

    if (!sudo_regex_compile(&re, pattern, &errstr)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unable to compile regular expression \"%s\": %s",
	    pattern, errstr);
	debug_return_bool(false);
    }

    errcode = regexec(&re, str, 0, NULL, 0);
    regfree(&re);

    debug_return_bool(errcode == 0);
}

static bool
command_args_match(const char *sudoers_cmnd, const char *sudoers_args)
{
    const char *args = user_args ? user_args : "";
    int flags = 0;
    debug_decl(command_args_match, SUDOERS_DEBUG_MATCH);

    /*
     * If no args specified in sudoers, any user args are allowed.
     * If the empty string is specified in sudoers, no user args are allowed.
     */
    if (sudoers_args == NULL)
	debug_return_bool(true);
    if (strcmp("\"\"", sudoers_args) == 0)
	debug_return_bool(user_args ? false : true);

    /*
     * If args are specified in sudoers, they must match the user args.
     * Args are matched either as a regular expression or glob pattern.
     */
    if (sudoers_args[0] == '^') {
	size_t len = strlen(sudoers_args);
	if (len > 0 && sudoers_args[len - 1] == '$')
	    debug_return_bool(regex_matches(sudoers_args, args));
    }

    /* If running as sudoedit, all args are assumed to be paths. */
    if (strcmp(sudoers_cmnd, "sudoedit") == 0)
	flags = FNM_PATHNAME;
    debug_return_bool(fnmatch(sudoers_args, args, flags) == 0);
}

#ifndef SUDOERS_NAME_MATCH
/*
 * Stat file by fd is possible, else by path.
 * Returns true on success, else false.
 */
static bool
do_stat(int fd, const char *path, struct stat *sb)
{
    bool ret;
    debug_decl(do_stat, SUDOERS_DEBUG_MATCH);

    if (fd != -1) {
	ret = fstat(fd, sb) == 0;
    } else {
	ret = stat(path, sb) == 0;
    }
    debug_return_bool(ret);
}

/*
 * Perform intercept-specific checks.
 * Returns true if allowed, else false.
 */
static bool
intercept_ok(const char *path, bool intercepted, struct stat *sb)
{
    debug_decl(intercept_ok, SUDOERS_DEBUG_MATCH);

    if (intercepted) {
	if (!def_intercept_allow_setid && ISSET(sb->st_mode, S_ISUID|S_ISGID)) {
	    sudo_debug_printf(SUDO_DEBUG_WARN|SUDO_DEBUG_LINENO,
		"rejecting setid command %s", path);
	    debug_return_bool(false);
	}
    }
    debug_return_bool(true);
}
#endif /* SUDOERS_NAME_MATCH */

/*
 * Check whether the fd refers to a shell script with a "#!" shebang.
 */
static bool
is_script(int fd)
{
    bool ret = false;
    char magic[2];
    debug_decl(is_script, SUDOERS_DEBUG_MATCH);

    if (pread(fd, magic, 2, 0) == 2) {
	if (magic[0] == '#' && magic[1] == '!')
	    ret = true;
    }
    debug_return_int(ret);
}

/*
 * Open path if fdexec is enabled or if a digest is present.
 * Returns false on error, else true.
 */
static bool
open_cmnd(const char *path, const struct command_digest_list *digests, int *fdp)
{
    int fd;
    debug_decl(open_cmnd, SUDOERS_DEBUG_MATCH);

    /* Only open the file for fdexec or for digest matching. */
    if (def_fdexec != always && TAILQ_EMPTY(digests))
	debug_return_bool(true);

    fd = open(path, O_RDONLY|O_NONBLOCK);
# ifdef O_EXEC
    if (fd == -1 && errno == EACCES && TAILQ_EMPTY(digests)) {
	/* Try again with O_EXEC if no digest is specified. */
	const int saved_errno = errno;
	if ((fd = open(path, O_EXEC)) == -1)
	    errno = saved_errno;
    }
# endif
    if (fd == -1)
	debug_return_bool(false);

    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    *fdp = fd;
    debug_return_bool(true);
}

static void
set_cmnd_fd(int fd, int rootfd)
{
    debug_decl(set_cmnd_fd, SUDOERS_DEBUG_MATCH);

    if (cmnd_fd != -1)
	close(cmnd_fd);

    if (fd != -1) {
	if (def_fdexec == never) {
	    /* Never use fexedcve() */
	    close(fd);
	    fd = -1;
	} else if (is_script(fd)) {
	    char fdpath[PATH_MAX];
	    struct stat sb;
	    int error, flags;

	    /* We can only use fexecve() on a script if /dev/fd/N exists. */
	    if (rootfd != -1) {
		/* Path relative to old root directory. */
		(void)snprintf(fdpath, sizeof(fdpath), "dev/fd/%d", fd);
		error = fstatat(rootfd, fdpath, &sb, 0);
	    } else {
		/* Absolute path. */
		(void)snprintf(fdpath, sizeof(fdpath), "/dev/fd/%d", fd);
		error = stat(fdpath, &sb);
	    }
	    if (error != 0) {
		/* Missing /dev/fd file, can't use fexecve(). */
		close(fd);
		fd = -1;
	    } else {
		/*
		 * Shell scripts go through namei twice so we can't have the
		 * close on exec flag set on the fd for fexecve(2).
		 */
		flags = fcntl(fd, F_GETFD) & ~FD_CLOEXEC;
		(void)fcntl(fd, F_SETFD, flags);
	    }
	}
    }

    cmnd_fd = fd;

    debug_return;
}

#ifndef SUDOERS_NAME_MATCH
/*
 * Return true if user_cmnd names one of the inodes in dir, else false.
 */
static bool
command_matches_dir(const char *sudoers_dir, size_t dlen, int rootfd,
    bool intercepted, const struct command_digest_list *digests)
{
    struct stat sudoers_stat;
    char path[PATH_MAX];
    int len, fd = -1;
    bool ret = false;
    debug_decl(command_matches_dir, SUDOERS_DEBUG_MATCH);

    /* Compare the canonicalized directories, if possible. */
    if (user_cmnd_dir != NULL) {
	char *resolved = canon_path(sudoers_dir);
	if (resolved != NULL) {
	    if (strcmp(resolved, user_cmnd_dir) != 0) {
		canon_path_free(resolved);
		goto done;
	    }
	    canon_path_free(resolved);
	}
    }

    /* Check for command in sudoers_dir. */
    len = snprintf(path, sizeof(path), "%s/%s", sudoers_dir, user_base);
    if (len < 0 || len >= ssizeof(path))
	goto done;

    /* Open the file for fdexec or for digest matching. */
    if (!open_cmnd(path, digests, &fd))
	goto done;
    if (!do_stat(fd, path, &sudoers_stat))
	goto done;
    if (!intercept_ok(path, intercepted, &sudoers_stat))
	goto done;

    if (user_stat == NULL ||
	(user_stat->st_dev == sudoers_stat.st_dev &&
	user_stat->st_ino == sudoers_stat.st_ino)) {
	if (!digest_matches(fd, path, digests))
	    goto done;
	free(safe_cmnd);
	if ((safe_cmnd = strdup(path)) == NULL) {
	    sudo_warnx(U_("%s: %s"), __func__,
		U_("unable to allocate memory"));
	}
	ret = true;
    }

done:
    if (fd != -1)
	close(fd);
    debug_return_bool(ret);
}
#else /* SUDOERS_NAME_MATCH */
/*
 * Return true if user_cmnd names one of the inodes in dir, else false.
 */
static bool
command_matches_dir(const char *sudoers_dir, size_t dlen, int rootfd,
    bool intercepted, const struct command_digest_list *digests)
{
    int fd = -1;
    debug_decl(command_matches_dir, SUDOERS_DEBUG_MATCH);

    /* Match user_cmnd against sudoers_dir. */
    if (strncmp(user_cmnd, sudoers_dir, dlen) != 0 || user_cmnd[dlen] != '/')
	goto bad;

    /* Make sure user_cmnd is not in a subdir of sudoers_dir. */
    if (strchr(user_cmnd + dlen + 1, '\0') != NULL)
	goto bad;

    /* Open the file for fdexec or for digest matching. */
    if (!open_cmnd(user_cmnd, digests, &fd))
	goto bad;
    if (!digest_matches(fd, user_cmnd, digests))
	goto bad;
    set_cmnd_fd(fd, rootfd);

    debug_return_bool(true);
bad:
    if (fd != -1)
	close(fd);
    debug_return_bool(false);
}
#endif /* SUDOERS_NAME_MATCH */

static bool
command_matches_all(int rootfd, bool intercepted,
    const struct command_digest_list *digests)
{
#ifndef SUDOERS_NAME_MATCH
    struct stat sb;
#endif
    int fd = -1;
    debug_decl(command_matches_all, SUDOERS_DEBUG_MATCH);

    if (strchr(user_cmnd, '/') != NULL) {
#ifndef SUDOERS_NAME_MATCH
	/* Open the file for fdexec or for digest matching. */
	bool open_error = !open_cmnd(user_cmnd, digests, &fd);

	/* A non-existent file is not an error for "sudo ALL". */
	if (do_stat(fd, user_cmnd, &sb)) {
	    if (open_error) {
		/* File exists but we couldn't open it above? */
		goto bad;
	    }
	    if (!intercept_ok(user_cmnd, intercepted, &sb))
		goto bad;
	}
#else
	/* Open the file for fdexec or for digest matching. */
	(void)open_cmnd(user_cmnd, digests, &fd);
#endif
    }

    /* Check digest of user_cmnd since we have no sudoers_cmnd for ALL. */
    if (!digest_matches(fd, user_cmnd, digests))
	goto bad;
    set_cmnd_fd(fd, rootfd);

    /* No need to set safe_cmnd for ALL. */
    debug_return_bool(true);
bad:
    if (fd != -1)
	close(fd);
    debug_return_bool(false);
}

static bool
command_matches_fnmatch(const char *sudoers_cmnd, const char *sudoers_args,
    int rootfd, bool intercepted, const struct command_digest_list *digests)
{
    const char *cmnd = user_cmnd;
    char buf[PATH_MAX];
    int len, fd = -1;
#ifndef SUDOERS_NAME_MATCH
    struct stat sb;
#endif
    debug_decl(command_matches_fnmatch, SUDOERS_DEBUG_MATCH);

    /* A relative user_cmnd will not match, try canonicalized version. */
    if (user_cmnd[0] != '/') {
	if (user_cmnd_dir == NULL)
	    debug_return_bool(false);
	len = snprintf(buf, sizeof(buf), "%s/%s", user_cmnd_dir,
	    user_base);
	if (len < 0 || len >= ssizeof(buf))
	    debug_return_bool(false);
	cmnd = buf;
    }

    /*
     * Return true if fnmatch(3) succeeds AND
     *  a) there are no args in sudoers OR
     *  b) there are no args on command line and none required by sudoers OR
     *  c) there are args in sudoers and on command line and they match
     *     else return false.
     */
    if (fnmatch(sudoers_cmnd, cmnd, FNM_PATHNAME) != 0)
	debug_return_bool(false);

    if (command_args_match(sudoers_cmnd, sudoers_args)) {
	/* Open the file for fdexec or for digest matching. */
	if (!open_cmnd(cmnd, digests, &fd))
	    goto bad;
#ifndef SUDOERS_NAME_MATCH
	if (!do_stat(fd, cmnd, &sb))
	    goto bad;
	if (!intercept_ok(cmnd, intercepted, &sb))
	    goto bad;
#endif
	/* Check digest of cmnd since sudoers_cmnd is a pattern. */
	if (!digest_matches(fd, cmnd, digests))
	    goto bad;
	set_cmnd_fd(fd, rootfd);

	/* No need to set safe_cmnd since cmnd matches sudoers_cmnd */
	debug_return_bool(true);
bad:
	if (fd != -1)
	    close(fd);
	debug_return_bool(false);
    }
    debug_return_bool(false);
}

static bool
command_matches_regex(const char *sudoers_cmnd, const char *sudoers_args,
    int rootfd, bool intercepted, const struct command_digest_list *digests)
{
    const char *cmnd = user_cmnd;
    char buf[PATH_MAX];
    int len, fd = -1;
#ifndef SUDOERS_NAME_MATCH
    struct stat sb;
#endif
    debug_decl(command_matches_regex, SUDOERS_DEBUG_MATCH);

    /* A relative user_cmnd will not match, try canonicalized version. */
    if (user_cmnd[0] != '/') {
	if (user_cmnd_dir == NULL)
	    debug_return_bool(false);
	len = snprintf(buf, sizeof(buf), "%s/%s", user_cmnd_dir,
	    user_base);
	if (len < 0 || len >= ssizeof(buf))
	    debug_return_bool(false);
	cmnd = buf;
    }

    /*
     * Return true if sudoers_cmnd regex matches cmnd AND
     *  a) there are no args in sudoers OR
     *  b) there are no args on command line and none required by sudoers OR
     *  c) there are args in sudoers and on command line and they match
     *     else return false.
     */
    if (!regex_matches(sudoers_cmnd, cmnd))
	debug_return_bool(false);

    if (command_args_match(sudoers_cmnd, sudoers_args)) {
	/* Open the file for fdexec or for digest matching. */
	if (!open_cmnd(cmnd, digests, &fd))
	    goto bad;
#ifndef SUDOERS_NAME_MATCH
	if (!do_stat(fd, cmnd, &sb))
	    goto bad;
	if (!intercept_ok(cmnd, intercepted, &sb))
	    goto bad;
#endif
	/* Check digest of cmnd since sudoers_cmnd is a pattern. */
	if (!digest_matches(fd, cmnd, digests))
	    goto bad;
	set_cmnd_fd(fd, rootfd);

	/* No need to set safe_cmnd since cmnd matches sudoers_cmnd */
	debug_return_bool(true);
bad:
	if (fd != -1)
	    close(fd);
	debug_return_bool(false);
    }
    debug_return_bool(false);
}

#ifndef SUDOERS_NAME_MATCH
static bool
command_matches_glob(const char *sudoers_cmnd, const char *sudoers_args,
    int rootfd, bool intercepted, const struct command_digest_list *digests)
{
    struct stat sudoers_stat;
    bool bad_digest = false;
    char **ap, *base, *cp;
    int fd = -1;
    size_t dlen;
    glob_t gl;
    debug_decl(command_matches_glob, SUDOERS_DEBUG_MATCH);

    /*
     * First check to see if we can avoid the call to glob(3).
     * Short circuit if there are no meta chars in the command itself
     * and user_base and basename(sudoers_cmnd) don't match.
     */
    dlen = strlen(sudoers_cmnd);
    if (sudoers_cmnd[dlen - 1] != '/') {
	base = sudo_basename(sudoers_cmnd);
	if (!has_meta(base) && strcmp(user_base, base) != 0)
	    debug_return_bool(false);
    }

    /*
     * Return true if we find a match in the glob(3) results AND
     *  a) there are no args in sudoers OR
     *  b) there are no args on command line and none required by sudoers OR
     *  c) there are args in sudoers and on command line and they match
     * else return false.
     */
    if (glob(sudoers_cmnd, GLOB_NOSORT, NULL, &gl) != 0 || gl.gl_pathc == 0) {
	globfree(&gl);
	debug_return_bool(false);
    }

    /* If user_cmnd is fully-qualified, check for an exact match. */
    if (user_cmnd[0] == '/') {
	for (ap = gl.gl_pathv; (cp = *ap) != NULL; ap++) {
	    if (fd != -1) {
		close(fd);
		fd = -1;
	    }

	    if (strcmp(cp, user_cmnd) != 0)
		continue;
	    /* Open the file for fdexec or for digest matching. */
	    if (!open_cmnd(cp, digests, &fd))
		continue;
	    if (!do_stat(fd, cp, &sudoers_stat))
		continue;
	    if (!intercept_ok(cp, intercepted, &sudoers_stat))
		continue;
	    if (user_stat == NULL ||
		(user_stat->st_dev == sudoers_stat.st_dev &&
		user_stat->st_ino == sudoers_stat.st_ino)) {
		/* There could be multiple matches, check digest early. */
		if (!digest_matches(fd, cp, digests)) {
		    bad_digest = true;
		    continue;
		}
		free(safe_cmnd);
		if ((safe_cmnd = strdup(cp)) == NULL) {
		    sudo_warnx(U_("%s: %s"), __func__,
			U_("unable to allocate memory"));
		    cp = NULL;		/* fail closed */
		}
	    } else {
		/* Paths match, but st_dev and st_ino are different. */
		cp = NULL;		/* fail closed */
	    }
	    goto done;
	}
    }
    /* No exact match, compare basename, user_cmnd_dir, st_dev and st_ino. */
    if (!bad_digest) {
	for (ap = gl.gl_pathv; (cp = *ap) != NULL; ap++) {
	    if (fd != -1) {
		close(fd);
		fd = -1;
	    }

	    /* If it ends in '/' it is a directory spec. */
	    dlen = strlen(cp);
	    if (cp[dlen - 1] == '/') {
		if (command_matches_dir(cp, dlen, rootfd, intercepted, digests)) {
		    globfree(&gl);
		    debug_return_bool(true);
		}
		continue;
	    }

	    /* Only proceed if user_base and basename(cp) match */
	    base = sudo_basename(cp);
	    if (strcmp(user_base, base) != 0)
		continue;

	    /* Compare the canonicalized parent directories, if possible. */
	    if (user_cmnd_dir != NULL) {
		char *slash = strchr(cp, '/');
		if (slash != NULL) {
		    char *resolved;
		    *slash = '\0';
		    resolved = canon_path(cp);
		    *slash = '/';
		    if (resolved != NULL) {
			/* Canonicalized directories must match. */
			int result = strcmp(resolved, user_cmnd_dir);
			canon_path_free(resolved);
			if (result != 0)
			    continue;
		    }
		}
	    }

	    /* Open the file for fdexec or for digest matching. */
	    if (!open_cmnd(cp, digests, &fd))
		continue;
	    if (!do_stat(fd, cp, &sudoers_stat))
		continue;
	    if (!intercept_ok(cp, intercepted, &sudoers_stat))
		continue;
	    if (user_stat == NULL ||
		(user_stat->st_dev == sudoers_stat.st_dev &&
		user_stat->st_ino == sudoers_stat.st_ino)) {
		if (!digest_matches(fd, cp, digests))
		    continue;
		free(safe_cmnd);
		if ((safe_cmnd = strdup(cp)) == NULL) {
		    sudo_warnx(U_("%s: %s"), __func__,
			U_("unable to allocate memory"));
		    cp = NULL;		/* fail closed */
		}
		goto done;
	    }
	}
    }
done:
    globfree(&gl);
    if (cp != NULL) {
	if (command_args_match(sudoers_cmnd, sudoers_args)) {
	    /* safe_cmnd was set above. */
	    set_cmnd_fd(fd, rootfd);
	    debug_return_bool(true);
	}
    }
    if (fd != -1)
	close(fd);
    debug_return_bool(false);
}

static bool
command_matches_normal(const char *sudoers_cmnd, const char *sudoers_args,
    int rootfd, bool intercepted, const struct command_digest_list *digests)
{
    struct stat sudoers_stat;
    const char *base;
    size_t dlen;
    int fd = -1;
    debug_decl(command_matches_normal, SUDOERS_DEBUG_MATCH);

    /* If it ends in '/' it is a directory spec. */
    dlen = strlen(sudoers_cmnd);
    if (sudoers_cmnd[dlen - 1] == '/') {
	debug_return_bool(command_matches_dir(sudoers_cmnd, dlen, rootfd,
	    intercepted, digests));
    }

    /* Only proceed if user_base and basename(sudoers_cmnd) match */
    base = sudo_basename(sudoers_cmnd);
    if (strcmp(user_base, base) != 0)
	debug_return_bool(false);

    /* Compare the canonicalized parent directories, if possible. */
    if (user_cmnd_dir != NULL) {
	const char *slash = strrchr(sudoers_cmnd, '/');
	if (slash != NULL) {
	    char sudoers_cmnd_dir[PATH_MAX], *resolved;
	    const size_t len = slash - sudoers_cmnd;
	    if (len >= sizeof(sudoers_cmnd_dir))
		goto bad;
	    if (len != 0)
		memcpy(sudoers_cmnd_dir, sudoers_cmnd, len);
	    sudoers_cmnd_dir[len] = '\0';
	    resolved = canon_path(sudoers_cmnd_dir);
	    if (resolved != NULL) {
		if (strcmp(resolved, user_cmnd_dir) != 0) {
		    canon_path_free(resolved);
		    goto bad;
		}
		canon_path_free(resolved);
	    }
	}
    }

    /* Open the file for fdexec or for digest matching. */
    if (!open_cmnd(sudoers_cmnd, digests, &fd))
	goto bad;

    /*
     * Return true if command matches AND
     *  a) there are no args in sudoers OR
     *  b) there are no args on command line and none req by sudoers OR
     *  c) there are args in sudoers and on command line and they match
     *  d) there is a digest and it matches
     */
    if (user_stat != NULL && do_stat(fd, sudoers_cmnd, &sudoers_stat)) {
	if (!intercept_ok(sudoers_cmnd, intercepted, &sudoers_stat))
	    goto bad;
	if (user_stat->st_dev != sudoers_stat.st_dev ||
	    user_stat->st_ino != sudoers_stat.st_ino)
	    goto bad;
    } else {
	/* Either user or sudoers command does not exist, match by name. */
	if (strcmp(user_cmnd, sudoers_cmnd) != 0)
	    goto bad;
    }
    if (!command_args_match(sudoers_cmnd, sudoers_args))
	goto bad;
    if (!digest_matches(fd, sudoers_cmnd, digests)) {
	/* XXX - log functions not available but we should log very loudly */
	goto bad;
    }
    free(safe_cmnd);
    if ((safe_cmnd = strdup(sudoers_cmnd)) == NULL) {
	sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	goto bad;
    }
    set_cmnd_fd(fd, rootfd);
    debug_return_bool(true);
bad:
    if (fd != -1)
	close(fd);
    debug_return_bool(false);
}
#else /* SUDOERS_NAME_MATCH */
static bool
command_matches_glob(const char *sudoers_cmnd, const char *sudoers_args,
    int rootfd, bool intercepted, const struct command_digest_list *digests)
{
    return command_matches_fnmatch(sudoers_cmnd, sudoers_args, rootfd,
	intercepted, digests);
}

static bool
command_matches_normal(const char *sudoers_cmnd, const char *sudoers_args,
    int rootfd, bool intercepted, const struct command_digest_list *digests)
{
    size_t dlen;
    int fd = -1;
    debug_decl(command_matches_normal, SUDOERS_DEBUG_MATCH);

    /* If it ends in '/' it is a directory spec. */
    dlen = strlen(sudoers_cmnd);
    if (sudoers_cmnd[dlen - 1] == '/') {
	debug_return_bool(command_matches_dir(sudoers_cmnd, dlen, rootfd,
	    intercepted, digests));
    }

    if (strcmp(user_cmnd, sudoers_cmnd) == 0) {
	if (command_args_match(sudoers_cmnd, sudoers_args)) {
	    /* Open the file for fdexec or for digest matching. */
	    if (!open_cmnd(user_cmnd, digests, &fd))
		goto bad;
	    if (!digest_matches(fd, user_cmnd, digests))
		goto bad;

	    /* Successful match. */
	    free(safe_cmnd);
	    if ((safe_cmnd = strdup(sudoers_cmnd)) == NULL) {
		sudo_warnx(U_("%s: %s"), __func__,
		    U_("unable to allocate memory"));
		goto bad;
	    }
	    set_cmnd_fd(fd, rootfd);
	    debug_return_bool(true);
	}
    }
bad:
    if (fd != -1)
	close(fd);
    debug_return_bool(false);
}
#endif /* SUDOERS_NAME_MATCH */

/*
 * If path doesn't end in /, return true iff cmnd & path name the same inode;
 * otherwise, return true if user_cmnd names one of the inodes in path.
 */
bool
command_matches(const char *sudoers_cmnd, const char *sudoers_args,
    const char *runchroot, struct cmnd_info *info,
    const struct command_digest_list *digests)
{
    const bool intercepted = info ? info->intercepted : false;
    int pivot_fds[2] = { -1, -1 };
    char *saved_user_cmnd = NULL;
    struct stat saved_user_stat;
    bool reset_cmnd = false;
    bool rc = false;
    debug_decl(command_matches, SUDOERS_DEBUG_MATCH);

    if (user_runchroot != NULL) {
	if (runchroot != NULL && strcmp(runchroot, "*") != 0 &&
		strcmp(runchroot, user_runchroot) != 0) {
	    /* CHROOT mismatch */
	    goto done;
	}
	/* User-specified runchroot (user_stat already set appropriately). */
	runchroot = user_runchroot;
    } else if (runchroot == NULL) {
	/* No rule-specific runchroot, use global (user_stat already set). */
	if (def_runchroot != NULL && strcmp(def_runchroot, "*") != '\0')
	    runchroot = def_runchroot;
    } else {
	/* Rule-specific runchroot, must reset user_cmnd and user_stat. */
	reset_cmnd = true;
    }

    /* Pivot root. */
    if (runchroot != NULL) {
	if (!pivot_root(runchroot, pivot_fds))
	    goto done;
    }

    if (reset_cmnd) {
	/* Rule-specific runchroot, set user_cmnd and user_stat after pivot. */
	int status;

	saved_user_cmnd = user_cmnd;
	if (user_stat != NULL)
	    saved_user_stat = *user_stat;
	status = set_cmnd_path(NULL);
	if (status != FOUND)
	    saved_user_cmnd = NULL;
	if (info != NULL)
	    info->status = status;
    }

    if (sudoers_cmnd == NULL) {
	sudoers_cmnd = "ALL";
	rc = command_matches_all(pivot_fds[0], intercepted, digests);
	goto done;
    }

    /* Check for regular expressions first. */
    if (sudoers_cmnd[0] == '^') {
	rc = command_matches_regex(sudoers_cmnd, sudoers_args, pivot_fds[0],
	    intercepted, digests);
	goto done;
    }

    /* Check for pseudo-commands */
    if (sudoers_cmnd[0] != '/') {
	/*
	 * Return true if sudoers_cmnd and user_cmnd match a pseudo-command AND
	 *  a) there are no args in sudoers OR
	 *  b) there are no args on command line and none req by sudoers OR
	 *  c) there are args in sudoers and on command line and they match
	 */
	if (strcmp(sudoers_cmnd, "list") == 0 ||
		strcmp(sudoers_cmnd, "sudoedit") == 0) {
	    if (strcmp(user_cmnd, sudoers_cmnd) == 0 &&
		    command_args_match(sudoers_cmnd, sudoers_args)) {
		/* No need to set safe_cmnd since user_cmnd == sudoers_cmnd */
		rc = true;
	    }
	}
	goto done;
    }

    if (has_meta(sudoers_cmnd)) {
	/*
	 * If sudoers_cmnd has meta characters in it, we need to
	 * use glob(3) and/or fnmatch(3) to do the matching.
	 */
	if (def_fast_glob) {
	    rc = command_matches_fnmatch(sudoers_cmnd, sudoers_args,
		pivot_fds[0], intercepted, digests);
	} else {
	    rc = command_matches_glob(sudoers_cmnd, sudoers_args, pivot_fds[0],
		intercepted, digests);
	}
    } else {
	rc = command_matches_normal(sudoers_cmnd, sudoers_args, pivot_fds[0],
	    intercepted, digests);
    }
done:
    /* Restore root. */
    if (runchroot != NULL)
	(void)unpivot_root(pivot_fds);

    /* Restore user_cmnd and user_stat. */
    if (saved_user_cmnd != NULL) {
	if (info != NULL) {
	    info->cmnd_path = user_cmnd;
	    if (user_stat != NULL)
		info->cmnd_stat = *user_stat;
	} else {
	    free(user_cmnd);
	}
	user_cmnd = saved_user_cmnd;
	if (user_stat != NULL)
	    *user_stat = saved_user_stat;
    }
    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
	"user command \"%s%s%s\" matches sudoers command \"%s%s%s\"%s%s: %s",
	user_cmnd, user_args ? " " : "", user_args ? user_args : "",
	sudoers_cmnd, sudoers_args ? " " : "", sudoers_args ? sudoers_args : "",
	runchroot ? ", chroot " : "", runchroot ? runchroot : "",
	rc ? "true" : "false");
    debug_return_bool(rc);
}
