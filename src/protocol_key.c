/*
    protocol_key.c -- handle the meta-protocol, key exchange
    Copyright (C) 1999-2003 Ivo Timmermans <ivo@o2w.nl>,
                  2000-2003 Guus Sliepen <guus@sliepen.eu.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id: protocol_key.c,v 1.1.4.19 2003/07/17 15:06:26 guus Exp $
*/

#include "system.h"

#include "avl_tree.h"
#include "connection.h"
#include "logger.h"
#include "net.h"
#include "netutl.h"
#include "node.h"
#include "protocol.h"
#include "utils.h"
#include "xalloc.h"

int mykeyused = 0;

int send_key_changed(connection_t *c, node_t *n)
{
	cp();

	/* Only send this message if some other daemon requested our key previously.
	   This reduces unnecessary key_changed broadcasts.
	 */

	if(n == myself && !mykeyused)
		return 0;

	return send_request(c, "%d %lx %s", KEY_CHANGED, random(), n->name);
}

int key_changed_h(connection_t *c)
{
	char name[MAX_STRING_SIZE];
	node_t *n;

	cp();

	if(sscanf(c->buffer, "%*d %*x " MAX_STRING, name) != 1) {
		logger(LOG_ERR, _("Got bad %s from %s (%s)"), "KEY_CHANGED",
			   c->name, c->hostname);
		return -1;
	}

	if(seen_request(c->buffer))
		return 0;

	n = lookup_node(name);

	if(!n) {
		logger(LOG_ERR, _("Got %s from %s (%s) origin %s which does not exist"),
			   "KEY_CHANGED", c->name, c->hostname, name);
		return -1;
	}

	n->status.validkey = 0;
	n->status.waitingforkey = 0;

	/* Tell the others */

	forward_request(c);

	return 0;
}

int send_req_key(connection_t *c, node_t *from, node_t *to)
{
	cp();

	return send_request(c, "%d %s %s", REQ_KEY, from->name, to->name);
}

int req_key_h(connection_t *c)
{
	char from_name[MAX_STRING_SIZE];
	char to_name[MAX_STRING_SIZE];
	node_t *from, *to;

	cp();

	if(sscanf(c->buffer, "%*d " MAX_STRING " " MAX_STRING, from_name, to_name) != 2) {
		logger(LOG_ERR, _("Got bad %s from %s (%s)"), "REQ_KEY", c->name,
			   c->hostname);
		return -1;
	}

	from = lookup_node(from_name);

	if(!from) {
		logger(LOG_ERR, _("Got %s from %s (%s) origin %s which does not exist in our connection list"),
			   "REQ_KEY", c->name, c->hostname, from_name);
		return -1;
	}

	to = lookup_node(to_name);

	if(!to) {
		logger(LOG_ERR, _("Got %s from %s (%s) destination %s which does not exist in our connection list"),
			   "REQ_KEY", c->name, c->hostname, to_name);
		return -1;
	}

	/* Check if this key request is for us */

	if(to == myself) {			/* Yes, send our own key back */
		mykeyused = 1;
		from->received_seqno = 0;
		memset(from->late, 0, sizeof(from->late));
		send_ans_key(c, myself, from);
	} else {
		send_req_key(to->nexthop->connection, from, to);
	}

	return 0;
}

int send_ans_key(connection_t *c, node_t *from, node_t *to)
{
	char key[MAX_STRING_SIZE];

	cp();

	bin2hex(from->key, key, from->keylength);
	key[from->keylength * 2] = '\0';

	return send_request(c, "%d %s %s %s %d %d %d %d", ANS_KEY,
						from->name, to->name, key,
						from->cipher ? from->cipher->nid : 0,
						from->digest ? from->digest->type : 0, from->maclength,
						from->compression);
}

int ans_key_h(connection_t *c)
{
	char from_name[MAX_STRING_SIZE];
	char to_name[MAX_STRING_SIZE];
	char key[MAX_STRING_SIZE];
	int cipher, digest, maclength, compression;
	node_t *from, *to;

	cp();

	if(sscanf(c->buffer, "%*d "MAX_STRING" "MAX_STRING" "MAX_STRING" %d %d %d %d",
		from_name, to_name, key, &cipher, &digest, &maclength,
		&compression) != 7) {
		logger(LOG_ERR, _("Got bad %s from %s (%s)"), "ANS_KEY", c->name,
			   c->hostname);
		return -1;
	}

	from = lookup_node(from_name);

	if(!from) {
		logger(LOG_ERR, _("Got %s from %s (%s) origin %s which does not exist in our connection list"),
			   "ANS_KEY", c->name, c->hostname, from_name);
		return -1;
	}

	to = lookup_node(to_name);

	if(!to) {
		logger(LOG_ERR, _("Got %s from %s (%s) destination %s which does not exist in our connection list"),
			   "ANS_KEY", c->name, c->hostname, to_name);
		return -1;
	}

	/* Forward it if necessary */

	if(to != myself) {
		return send_request(to->nexthop->connection, "%s", c->buffer);
	}

	/* Update our copy of the origin's packet key */

	if(from->key)
		free(from->key);

	from->key = xstrdup(key);
	from->keylength = strlen(key) / 2;
	hex2bin(from->key, from->key, from->keylength);
	from->key[from->keylength] = '\0';

	from->status.validkey = 1;
	from->status.waitingforkey = 0;
	from->sent_seqno = 0;

	/* Check and lookup cipher and digest algorithms */

	if(cipher) {
		from->cipher = EVP_get_cipherbynid(cipher);

		if(!from->cipher) {
			logger(LOG_ERR, _("Node %s (%s) uses unknown cipher!"), from->name,
				   from->hostname);
			return -1;
		}

		if(from->keylength != from->cipher->key_len + from->cipher->iv_len) {
			logger(LOG_ERR, _("Node %s (%s) uses wrong keylength!"), from->name,
				   from->hostname);
			return -1;
		}
	} else {
		from->cipher = NULL;
	}

	from->maclength = maclength;

	if(digest) {
		from->digest = EVP_get_digestbynid(digest);

		if(!from->digest) {
			logger(LOG_ERR, _("Node %s (%s) uses unknown digest!"), from->name,
				   from->hostname);
			return -1;
		}

		if(from->maclength > from->digest->md_size || from->maclength < 0) {
			logger(LOG_ERR, _("Node %s (%s) uses bogus MAC length!"),
				   from->name, from->hostname);
			return -1;
		}
	} else {
		from->digest = NULL;
	}

	if(compression < 0 || compression > 11) {
		logger(LOG_ERR, _("Node %s (%s) uses bogus compression level!"), from->name, from->hostname);
		return -1;
	}
	
	from->compression = compression;

	EVP_EncryptInit_ex(&from->packet_ctx, from->cipher, NULL, from->key, from->key + from->cipher->key_len);

	flush_queue(from);

	return 0;
}
