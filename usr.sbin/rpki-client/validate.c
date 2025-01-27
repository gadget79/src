/*	$OpenBSD: validate.c,v 1.17 2021/10/24 12:06:16 job Exp $ */
/*
 * Copyright (c) 2019 Kristaps Dzonsons <kristaps@bsd.lv>
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
 */

#include <sys/socket.h>

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "extern.h"

static void
tracewarn(const struct auth *a)
{

	for (; a != NULL; a = a->parent)
		warnx(" ...inheriting from: %s", a->fn);
}

/*
 * Walk up the chain of certificates trying to match our AS number to
 * one of the allocations in that chain.
 * Returns 1 if covered or 0 if not.
 */
static int
valid_as(struct auth *a, uint32_t min, uint32_t max)
{
	int	 c;

	if (a == NULL)
		return 0;

	/* Does this certificate cover our AS number? */
	if (a->cert->asz) {
		c = as_check_covered(min, max, a->cert->as, a->cert->asz);
		if (c > 0)
			return 1;
		else if (c < 0)
			return 0;
	}

	/* If it doesn't, walk up the chain. */
	return valid_as(a->parent, min, max);
}

/*
 * Walk up the chain of certificates (really just the last one, but in
 * the case of inheritence, the ones before) making sure that our IP
 * prefix is covered in the first non-inheriting specification.
 * Returns 1 if covered or 0 if not.
 */
static int
valid_ip(struct auth *a, enum afi afi,
    const unsigned char *min, const unsigned char *max)
{
	int	 c;

	if (a == NULL)
		return 0;

	/* Does this certificate cover our IP prefix? */
	c = ip_addr_check_covered(afi, min, max, a->cert->ips, a->cert->ipsz);
	if (c > 0)
		return 1;
	else if (c < 0)
		return 0;

	/* If it doesn't, walk up the chain. */
	return valid_ip(a->parent, afi, min, max);
}

/*
 * Make sure that the SKI doesn't already exist and return the parent by
 * its AKI.
 * Returns the parent auth or NULL on failure.
 */
struct auth *
valid_ski_aki(const char *fn, struct auth_tree *auths,
    const char *ski, const char *aki)
{
	struct auth *a;

	if (auth_find(auths, ski) != NULL) {
		warnx("%s: RFC 6487: duplicate SKI", fn);
		return NULL;
	}

	a = auth_find(auths, aki);
	if (a == NULL)
		warnx("%s: RFC 6487: unknown AKI", fn);

	return a;
}

/*
 * Authenticate a trust anchor by making sure its resources are not
 * inheriting and that the SKI is unique.
 * Returns 1 if valid, 0 otherwise.
 */
int
valid_ta(const char *fn, struct auth_tree *auths, const struct cert *cert)
{
	size_t	 i;

	/* AS and IP resources must not inherit. */
	if (cert->asz && cert->as[0].type == CERT_AS_INHERIT) {
		warnx("%s: RFC 6487 (trust anchor): "
		    "inheriting AS resources", fn);
		return 0;
	}
	for (i = 0; i < cert->ipsz; i++)
		if (cert->ips[i].type == CERT_IP_INHERIT) {
			warnx("%s: RFC 6487 (trust anchor): "
			    "inheriting IP resources", fn);
			return 0;
		}

	/* SKI must not be a dupe. */
	if (auth_find(auths, cert->ski) != NULL) {
		warnx("%s: RFC 6487: duplicate SKI", fn);
		return 0;
	}

	return 1;
}

/*
 * Validate a non-TA certificate: make sure its IP and AS resources are
 * fully covered by those in the authority key (which must exist).
 * Returns 1 if valid, 0 otherwise.
 */
int
valid_cert(const char *fn, struct auth_tree *auths, const struct cert *cert)
{
	struct auth	*a;
	size_t		 i;
	uint32_t	 min, max;
	char		 buf1[64], buf2[64];

	a = valid_ski_aki(fn, auths, cert->ski, cert->aki);
	if (a == NULL)
		return 0;

	for (i = 0; i < cert->asz; i++) {
		if (cert->as[i].type == CERT_AS_INHERIT) {
			if (cert->purpose == CERT_PURPOSE_BGPSEC_ROUTER)
				return 0; /* BGPsec doesn't permit inheriting */
			continue;
		}
		min = cert->as[i].type == CERT_AS_ID ?
		    cert->as[i].id : cert->as[i].range.min;
		max = cert->as[i].type == CERT_AS_ID ?
		    cert->as[i].id : cert->as[i].range.max;
		if (valid_as(a, min, max))
			continue;
		warnx("%s: RFC 6487: uncovered AS: "
		    "%u--%u", fn, min, max);
		tracewarn(a);
		return 0;
	}

	for (i = 0; i < cert->ipsz; i++) {
		if (valid_ip(a, cert->ips[i].afi, cert->ips[i].min,
		    cert->ips[i].max))
			continue;
		switch (cert->ips[i].type) {
		case CERT_IP_RANGE:
			ip_addr_print(&cert->ips[i].range.min,
			    cert->ips[i].afi, buf1, sizeof(buf1));
			ip_addr_print(&cert->ips[i].range.max,
			    cert->ips[i].afi, buf2, sizeof(buf2));
			warnx("%s: RFC 6487: uncovered IP: "
			    "%s--%s", fn, buf1, buf2);
			break;
		case CERT_IP_ADDR:
			ip_addr_print(&cert->ips[i].ip,
			    cert->ips[i].afi, buf1, sizeof(buf1));
			warnx("%s: RFC 6487: uncovered IP: "
			    "%s", fn, buf1);
			break;
		case CERT_IP_INHERIT:
			warnx("%s: RFC 6487: uncovered IP: "
			    "(inherit)", fn);
			break;
		}
		tracewarn(a);
		return 0;
	}

	return 1;
}

/*
 * Validate our ROA: check that the SKI is unique, the AKI exists, and
 * the IP prefix is also contained.
 * Returns 1 if valid, 0 otherwise.
 */
int
valid_roa(const char *fn, struct auth_tree *auths, struct roa *roa)
{
	struct auth	*a;
	size_t	 i;
	char	 buf[64];

	a = valid_ski_aki(fn, auths, roa->ski, roa->aki);
	if (a == NULL)
		return 0;

	if ((roa->tal = strdup(a->tal)) == NULL)
		err(1, NULL);

	for (i = 0; i < roa->ipsz; i++) {
		if (valid_ip(a, roa->ips[i].afi, roa->ips[i].min,
		    roa->ips[i].max))
			continue;
		ip_addr_print(&roa->ips[i].addr,
		    roa->ips[i].afi, buf, sizeof(buf));
		warnx("%s: RFC 6482: uncovered IP: "
		    "%s", fn, buf);
		tracewarn(a);
		return 0;
	}

	return 1;
}

/*
 * Validate a filename listed on a Manifest.
 * draft-ietf-sidrops-6486bis section 4.2.2
 * Returns 1 if filename is valid, otherwise 0.
 */
int
valid_filename(const char *fn)
{
	size_t			 sz;
	const unsigned char	*c;

	sz = strlen(fn);
	if (sz < 5)
		return 0;

	for (c = fn; *c != '\0'; ++c)
		if (!isalnum(*c) && *c != '-' && *c != '_' && *c != '.')
			return 0;

	if (strchr(fn, '.') != strrchr(fn, '.'))
		return 0;

	if (strcasecmp(fn + sz - 4, ".cer") == 0)
		return 1;
	if (strcasecmp(fn + sz - 4, ".crl") == 0)
		return 1;
	if (strcasecmp(fn + sz - 4, ".gbr") == 0)
		return 1;
	if (strcasecmp(fn + sz - 4, ".roa") == 0)
		return 1;

	return 0;
}

/*
 * Validate a file by verifying the SHA256 hash of that file.
 * Returns 1 if valid, 0 otherwise.
 */
int
valid_filehash(const char *fn, const char *hash, size_t hlen)
{
	SHA256_CTX ctx;
	char	filehash[SHA256_DIGEST_LENGTH];
	char	buffer[8192];
	ssize_t	nr;
	int	fd;

	if (hlen != sizeof(filehash))
		errx(1, "bad hash size");

	if ((fd = open(fn, O_RDONLY)) == -1)
		return 0;

	SHA256_Init(&ctx);
	while ((nr = read(fd, buffer, sizeof(buffer))) > 0)
		SHA256_Update(&ctx, buffer, nr);
	close(fd);

	SHA256_Final(filehash, &ctx);
	if (memcmp(hash, filehash, sizeof(filehash)) != 0)
		return 0;

	return 1;
}

/*
 * Validate a URI to make sure it is pure ASCII and does not point backwards
 * or doing some other silly tricks. To enforce the protocol pass either
 * https:// or rsync:// as proto, if NULL is passed no protocol is enforced.
 * Returns 1 if valid, 0 otherwise.
 */
int
valid_uri(const char *uri, size_t usz, const char *proto)
{
	size_t s;

	for (s = 0; s < usz; s++)
		if (!isalnum((unsigned char)uri[s]) &&
		    !ispunct((unsigned char)uri[s]))
			return 0;

	if (proto != NULL) {
		s = strlen(proto);
		if (strncasecmp(uri, proto, s) != 0)
			return 0;
	}

	/* do not allow files or directories to start with a '.' */
	if (strstr(uri, "/.") != NULL)
		return 0;

	return 1;
}
