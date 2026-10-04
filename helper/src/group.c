/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vitrine-helper setup-group: the caller in the vitrine group, whose members
 * polkit lets use the helper without a password (49-vitrine.rules).
 *
 * vitrine offers it when a VM starts untuned because its user is not in the
 * group.  pkexec runs it under an action of its own,
 * org.vitrine.helper.setup-group: an administrator's password, typed in the
 * desktop's polkit dialog, for everyone (the group's rule does not grant
 * it).  It takes no argument: the group is "vitrine" and the user is the
 * caller (pkexec's PKEXEC_UID, from the user database), never anyone else.
 * It creates the group as a system group when there is none, adds the
 * caller to it, and changes nothing when both are done already.  A group
 * named vitrine that it did not create - another user's private group, one
 * sharing the id of disk or wheel - is not joined: the caller would get that
 * group's access to files, which the administrator's dialog does not say.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "helper.h"

static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char *fmt, ...)
{
    char why[400];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    reply("error setup-group: %s", why);
    return 1;
}

/* A name the group tools take as a user's, never as an option */
static bool plain_user(const char *name)
{
    size_t len = strlen(name);

    if (len == 0 || len > 64 || name[0] == '-') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || (c == '$' && i == len - 1);
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* The highest id groupadd --system gives: login.defs' SYS_GID_MAX */
static unsigned long sys_gid_max(void)
{
    char path[PATH_MAX], line[256], key[32];
    unsigned long max = 999, v;
    FILE *f;

    if (!rooted(path, sizeof(path), "/etc/login.defs") || !(f = fopen(path, "re"))) {
        return max;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " %31s %lu", key, &v) == 2 && strcmp(key, "SYS_GID_MAX") == 0) {
            max = v;
        }
    }
    fclose(f);
    return max;
}

/*
 * The group VITRINE_GROUP, of id @gid, as setup-group would have created it:
 * a system group of /etc/group, alone with its id, and nobody's primary
 * group (not a user's private group).  NULL, or why not in @why.
 */
static const char *not_ours(gid_t gid, char *why, size_t size)
{
    char path[PATH_MAX];
    struct group *gr;
    struct passwd *pw;
    bool listed = false;
    FILE *f;

    if (gid == 0) {
        return "its id is 0";
    }
    if (gid > sys_gid_max()) {
        snprintf(why, size, "its id %u is not a system group's", (unsigned)gid);
        return why;
    }
    if (!rooted(path, sizeof(path), "/etc/group") || !(f = fopen(path, "re"))) {
        return "/etc/group cannot be read";
    }
    why[0] = '\0';
    while ((gr = fgetgrent(f))) {
        if (gr->gr_gid != gid) {
            continue;
        }
        if (strcmp(gr->gr_name, VITRINE_GROUP) == 0) {
            listed = true;
        } else if (!why[0]) {
            snprintf(why, size, "its id %u is also %.64s's", (unsigned)gid, gr->gr_name);
        }
    }
    fclose(f);
    if (why[0]) {
        return why;
    }
    if (!listed) {
        /* the user database's, from elsewhere (LDAP, sssd): not for gpasswd */
        return "it is not in /etc/group";
    }
    /* /etc/passwd's users, and the user database's vitrine */
    if (!rooted(path, sizeof(path), "/etc/passwd")) {
        return "/etc/passwd cannot be read";
    }
    if ((f = fopen(path, "re"))) {
        while ((pw = fgetpwent(f))) {
            if (pw->pw_gid == gid && !why[0]) {
                snprintf(why, size, "it is the primary group of %.64s", pw->pw_name);
            }
        }
        fclose(f);
    } else if (errno != ENOENT) {
        return "/etc/passwd cannot be read";
    }
    if (!why[0] && (pw = getpwnam(VITRINE_GROUP)) && pw->pw_gid == gid) {
        snprintf(why, size, "it is the primary group of " VITRINE_GROUP);
    }
    return why[0] ? why : NULL;
}

int setup_group(void)
{
    char user[65], err[256], why[128];
    const char *theirs;
    struct passwd *pw;
    gid_t primary, gid;
    bool created = false;

    if (caller_uid == 0) {
        return fail("root needs no group");
    }
    if (!(pw = getpwuid(caller_uid))) {
        return fail("uid %u is not in the user database", (unsigned)caller_uid);
    }
    if (!plain_user(pw->pw_name)) {
        return fail("the user name %.64s is not one the group tools take", pw->pw_name);
    }
    snprintf(user, sizeof(user), "%s", pw->pw_name);
    primary = pw->pw_gid;

    if (sys_group_find(VITRINE_GROUP, &gid) == 0) {
        if (sys_group_create(VITRINE_GROUP, err, sizeof(err)) < 0) {
            return fail("cannot create the group: %s", err);
        }
        created = true;
        sys_log("uid %u: created the group " VITRINE_GROUP, (unsigned)caller_uid);
        if (sys_group_find(VITRINE_GROUP, &gid) == 0) {
            return fail("the group was created, but the user database does not list it");
        }
    } else if (sys_group_has(user, primary, gid)) {
        reply("ok setup-group: %s is in " VITRINE_GROUP " already", user);
        return 0;
    } else if ((theirs = not_ours(gid, why, sizeof(why)))) {
        /* the same name, another group: joined, it would give its files */
        return fail("a group named " VITRINE_GROUP " exists that vitrine did not create: %s",
                    theirs);
    }
    if (sys_group_add_user(VITRINE_GROUP, user, err, sizeof(err)) < 0) {
        return fail("cannot add the user to the group: %s", err);
    }
    sys_log("uid %u: %s added to the group " VITRINE_GROUP, (unsigned)caller_uid, user);
    reply("ok setup-group: %s added to " VITRINE_GROUP "%s", user, created ? ", group created" : "");
    return 0;
}
