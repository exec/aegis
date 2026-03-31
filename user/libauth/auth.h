/* auth.h — shared authentication library for Aegis.
 * Used by /bin/login (text) and /bin/bastion (graphical). */
#ifndef AUTH_H
#define AUTH_H

/* Connect to capd and request a capability by kind number.
 * Returns 0 on success, -1 on failure. */
int capd_request(unsigned int kind);

/* Look up username in /etc/passwd.
 * Returns 0 on success, -1 on failure.
 * Fills uid, gid, home, shell from the matching entry. */
int auth_lookup_passwd(const char *username, int *uid, int *gid,
                       char *home, int homelen, char *shell, int shelllen);

/* Look up shadow hash for username in /etc/shadow.
 * Requires CAP_KIND_AUTH. Returns 0 on success, -1 on failure. */
int auth_lookup_shadow(const char *username, char *hash, int hashlen);

/* Verify password against a shadow hash using crypt().
 * Returns 0 on match, -1 on mismatch or error. */
int auth_verify(const char *password, const char *hash);

/* Full authentication check: passwd + shadow + crypt.
 * Returns 0 on success, -1 on failure.
 * On success, fills uid, gid, home (homelen), shell (shelllen). */
int auth_check(const char *username, const char *password,
               int *uid, int *gid,
               char *home, int homelen, char *shell, int shelllen);

/* Set process identity (setuid + setgid). */
void auth_set_identity(int uid, int gid);

/* Pre-register CAP_DELEGATE + CAP_QUERY as exec_caps for the next execve.
 * The shell inherits these capabilities. */
void auth_grant_shell_caps(void);

/* Request the standard auth capabilities from capd:
 * AUTH, CAP_GRANT, CAP_DELEGATE, CAP_QUERY, SETUID */
void auth_request_caps(void);

#endif /* AUTH_H */
