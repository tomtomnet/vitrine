/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vitrine-helper setup-group: the caller in the vitrine group, whose members
 * polkit lets use the helper without a password (49-vitrine.rules).
 *
 * vitrine offers it when a VM starts untuned because its user is not in the
 * group.  pkexec runs it under the helper's one polkit action: an
 * administrator's password, typed in the desktop's polkit dialog, for
 * anyone but the group's members - for whom it changes nothing.  It takes
 * no argument: the group is "vitrine" and the user is the caller (pkexec's
 * PKEXEC_UID, from the user database), never anyone else.  It creates the
 * group as a system group when there is none, adds the caller to it, and
 * changes nothing when both are done already.
 */
#define _GNU_SOURCE
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

int setup_group(void)
{
    char user[65], err[256];
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
    }
    /* a group root's by its id would give the caller far more than the helper */
    if (gid == 0) {
        return fail("the group " VITRINE_GROUP " has the id 0");
    }
    if (sys_group_add_user(VITRINE_GROUP, user, err, sizeof(err)) < 0) {
        return fail("cannot add the user to the group: %s", err);
    }
    sys_log("uid %u: %s added to the group " VITRINE_GROUP, (unsigned)caller_uid, user);
    reply("ok setup-group: %s added to " VITRINE_GROUP "%s", user, created ? ", group created" : "");
    return 0;
}
