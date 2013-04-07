/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * $Id$
 *
 * @file xlat.c
 * @brief String expansion ("translation"). Implements %Attribute -> value
 *
 * @copyright 2000,2006  The FreeRADIUS server project
 * @copyright 2000  Alan DeKok <aland@ox.org>
 */

RCSID("$Id$")

#include	<freeradius-devel/radiusd.h>
#include	<freeradius-devel/rad_assert.h>
#include	<freeradius-devel/base64.h>

#include	<ctype.h>

typedef struct xlat_t {
	char		module[MAX_STRING_LEN];
	int		length;
	void		*instance;
	RAD_XLAT_FUNC	do_xlat;
	int		internal;	/* not allowed to re-define these */
} xlat_t;

typedef struct xlat_exp xlat_exp_t;

struct xlat_exp {
	const char *fmt;	//!< The format string.
	size_t len;		//!< Length of the format string.
	
	xlat_exp_t *child;	//!< Nested expansion.
	xlat_exp_t *next;	//!< Next in the list.
	xlat_exp_t *alternate;	//!< Alternative expansion if this one expanded to a zero length string.		
	
	const xlat_t *xlat;	//!< The xlat expansion to expand format with.
};

typedef struct xlat_out {
	const char *out;	//!< Output data.
	size_t len;		//!< Length of the output string.
} xlat_out_t;

typedef enum {
	XLAT_LITERAL,		//!< Literal string
	XLAT_EXPANSION,		//!< xlat function
	XLAT_ALTERNATE		//!< xlat conditional syntax :-
} xlat_state_t;

static rbtree_t *xlat_root = NULL;

/*
 *	Define all xlat's in the structure.
 */
static const char * const internal_xlat[] = {"check",
					     "request",
					     "reply",
					     "proxy-request",
					     "proxy-reply",
					     "outer.request",
					     "outer.reply",
					     "outer.control",
					     NULL};
#ifdef WITH_UNLANG
static const char * const xlat_foreach_names[] = {"Foreach-Variable-0",
						  "Foreach-Variable-1",
						  "Foreach-Variable-2",
						  "Foreach-Variable-3",
						  "Foreach-Variable-4",
						  "Foreach-Variable-5",
						  "Foreach-Variable-6",
						  "Foreach-Variable-7",
						  "Foreach-Variable-8",
						  "Foreach-Variable-9",
						  NULL};
#endif

#if REQUEST_MAX_REGEX > 8
#error Please fix the following line
#endif
static int xlat_inst[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };	/* up to 8 for regex */

/** Convert the value on a VALUE_PAIR to string
 *
 */
static int valuepair2str(char * out,int outlen,VALUE_PAIR * pair, int type)
{
	if (pair != NULL) {
		vp_prints_value(out, outlen, pair, -1);
		return strlen(out);
	}

	switch (type) {
	case PW_TYPE_STRING :
		strlcpy(out,"_",outlen);
		break;
	case PW_TYPE_INTEGER64:
	case PW_TYPE_SIGNED:
	case PW_TYPE_INTEGER:
		strlcpy(out,"0",outlen);
		break;
	case PW_TYPE_IPADDR :
		strlcpy(out,"?.?.?.?",outlen);
		break;
	case PW_TYPE_IPV6ADDR :
		strlcpy(out,":?:",outlen);
		break;
	case PW_TYPE_DATE :
		strlcpy(out,"0",outlen);
		break;
	default :
		strlcpy(out,"unknown_type",outlen);
	}
	return strlen(out);
}

/*
 *	Dynamically translate for check:, request:, reply:, etc.
 */
static size_t xlat_packet(void *instance, REQUEST *request,
			  const char *fmt, char *out, size_t outlen)
{
	const DICT_ATTR	*da;
	VALUE_PAIR	*vp;
	VALUE_PAIR	*vps = NULL;
	RADIUS_PACKET	*packet = NULL;

	switch (*(int*) instance) {
	case 0:
		vps = request->config_items;
		break;

	case 1:
		vps = request->packet->vps;
		packet = request->packet;
		break;

	case 2:
		vps = request->reply->vps;
		packet = request->reply;
		break;

	case 3:
#ifdef WITH_PROXY
		if (request->proxy) vps = request->proxy->vps;
		packet = request->proxy;
#endif
		break;

	case 4:
#ifdef WITH_PROXY
		if (request->proxy_reply) vps = request->proxy_reply->vps;
		packet = request->proxy_reply;
#endif
		break;

	case 5:
		if (request->parent) {
			vps = request->parent->packet->vps;
			packet = request->parent->packet;
		}
		break;
			
	case 6:
		if (request->parent && request->parent->reply) {
			vps = request->parent->reply->vps;
			packet = request->parent->reply;
		}
		break;
			
	case 7:
		if (request->parent) {
			vps = request->parent->config_items;
		}
		break;
			
	default:		/* WTF? */
		return 0;
	}

	/*
	 *	The "format" string is the attribute name.
	 */
	da = dict_attrbyname(fmt);
	if (!da) {
		int do_number = FALSE;
		int do_array = FALSE;
		int do_count = FALSE;
		int do_all = FALSE;
		int tag = 0;
		size_t count = 0, total;
		char *p;
		char buffer[256];

		if (strlen(fmt) > sizeof(buffer)) return 0;

		strlcpy(buffer, fmt, sizeof(buffer));

		/*
		 *	%{Attribute-name#} - print integer version of it.
		 */
		p = buffer + strlen(buffer) - 1;
		if (*p == '#') {
			*p = '\0';
			do_number = TRUE;
		}

		/*
		 *	%{Attribute-Name:tag} - get the name with the specified
		 *	value of the tag.
		 */
		p = strchr(buffer, ':');
		if (p) {
			tag = atoi(p + 1);
			*p = '\0';
			p++;

		} else {
			/*
			 *	Allow %{Attribute-Name:tag[...]}
			 */
			p = buffer;
		}

		/*
		 *	%{Attribute-Name[...] does more stuff
		 */
		p = strchr(p, '[');
		if (p) {
			*p = '\0';
			do_array = TRUE;
			if (p[1] == '#') {
				do_count = TRUE;
			} else if (p[1] == '*') {
				do_all = TRUE;
			} else {
				count = atoi(p + 1);
				p += 1 + strspn(p + 1, "0123456789");
				if (*p != ']') {
					RDEBUG2("xlat: Invalid array reference in string at %s %s",
						fmt, p);
					return 0;
				}
			}
		}

		/*
		 *	We COULD argue about %{Attribute-Name[#]#} etc.
		 *	But that looks like more work than it's worth.
		 */

		da = dict_attrbyname(buffer);
		if (!da) return 0;

		/*
		 *	No array, print the tagged attribute.
		 */
		if (!do_array) {
			vp = pairfind(vps, da->attr, da->vendor, tag);
			goto just_print;
		}

		total = 0;

		/*
		 *	Array[#] - return the total
		 */
		if (do_count) {
			for (vp = pairfind(vps, da->attr, da->vendor, tag);
			     vp != NULL;
			     vp = pairfind(vp->next, da->attr, da->vendor, tag)) {
				total++;
			}

			snprintf(out, outlen, "%d", (int) total);
			return strlen(out);
		}

		/*
		 *	%{Attribute-Name[*]} returns ALL of the
		 *	the attributes, separated by a newline.
		 */
		if (do_all) {
			for (vp = pairfind(vps, da->attr, da->vendor, tag);
			     vp != NULL;
			     vp = pairfind(vp->next, da->attr, da->vendor, tag)) {
				count = valuepair2str(out, outlen - 1, vp, da->type);
				rad_assert(count <= outlen);
				total += count + 1;
				outlen -= (count + 1);
				out += count;

				*(out++) = '\n';

				if (outlen <= 1) break;
			}

			*out = '\0';
			return total;
		}

		/*
		 *	Find the N'th value.
		 */
		for (vp = pairfind(vps, da->attr, da->vendor, tag);
		     vp != NULL;
		     vp = pairfind(vp->next, da->attr, da->vendor, tag)) {
			if (total == count) break;
			total++;
			if (total > count) {
				vp = NULL;
				break;
			}
		}

		/*
		 *	Non-existent array reference.
		 */
	just_print:
		if (!vp) return 0;

		if (do_number) {
			if ((vp->da->type != PW_TYPE_IPADDR) &&
			    (vp->da->type != PW_TYPE_INTEGER) &&
			    (vp->da->type != PW_TYPE_SHORT) &&
			    (vp->da->type != PW_TYPE_BYTE) &&
			    (vp->da->type != PW_TYPE_DATE)) {
				*out = '\0';
				return 0;
			}
			
			return snprintf(out, outlen, "%u", vp->vp_integer);
		}

		return valuepair2str(out, outlen, vp, da->type);
	}

	vp = pairfind(vps, da->attr, da->vendor, TAG_ANY);
	if (!vp) {
		/*
		 *	Some "magic" handlers, which are never in VP's, but
		 *	which are in the packet.
		 *
		 *	@bug FIXME: We should really do this in a more
		 *	intelligent way...
		 */
		if (packet) {
			VALUE_PAIR localvp;

			memset(&localvp, 0, sizeof(localvp));

			switch (da->attr) {
			case PW_PACKET_TYPE:
			{
				DICT_VALUE *dval;

				dval = dict_valbyattr(da->attr, da->vendor, packet->code);
				if (dval) {
					snprintf(out, outlen, "%s", dval->name);
				} else {
					snprintf(out, outlen, "%d", packet->code);
				}
				return strlen(out);
			}
			break;

			case PW_CLIENT_SHORTNAME:
				if (request->client && request->client->shortname) {
					strlcpy(out, request->client->shortname, outlen);
				} else {
					strlcpy(out, "<UNKNOWN-CLIENT>", outlen);
				}
				return strlen(out);

			case PW_CLIENT_IP_ADDRESS: /* the same as below */
			case PW_PACKET_SRC_IP_ADDRESS:
				if (packet->src_ipaddr.af != AF_INET) {
					return 0;
				}

				localvp.da = da;
				localvp.vp_ipaddr = packet->src_ipaddr.ipaddr.ip4addr.s_addr;
				break;

			case PW_PACKET_DST_IP_ADDRESS:
				if (packet->dst_ipaddr.af != AF_INET) {
					return 0;
				}

				localvp.da = da;
				localvp.vp_ipaddr = packet->dst_ipaddr.ipaddr.ip4addr.s_addr;
				break;

			case PW_PACKET_SRC_PORT:

				localvp.da = da;
				localvp.vp_integer = packet->src_port;
				break;

			case PW_PACKET_DST_PORT:
				localvp.da = da;
				localvp.vp_integer = packet->dst_port;
				break;

			case PW_PACKET_AUTHENTICATION_VECTOR:
				localvp.da = da;
				memcpy(localvp.vp_strvalue, packet->vector,
				       sizeof(packet->vector));
				localvp.length = sizeof(packet->vector);
				break;

				/*
				 *	Authorization, accounting, etc.
				 */
			case PW_REQUEST_PROCESSING_STAGE:
				if (request->component) {
					strlcpy(out, request->component, outlen);
				} else {
					strlcpy(out, "server_core", outlen);
				}
				return strlen(out);

			case PW_PACKET_SRC_IPV6_ADDRESS:
				if (packet->src_ipaddr.af != AF_INET6) {
					return 0;
				}
				localvp.da = da;
				memcpy(localvp.vp_strvalue,
				       &packet->src_ipaddr.ipaddr.ip6addr,
				       sizeof(packet->src_ipaddr.ipaddr.ip6addr));
				break;

			case PW_PACKET_DST_IPV6_ADDRESS:
				if (packet->dst_ipaddr.af != AF_INET6) {
					return 0;
				}
				
				localvp.da = da;
				memcpy(localvp.vp_strvalue,
				       &packet->dst_ipaddr.ipaddr.ip6addr,
				       sizeof(packet->dst_ipaddr.ipaddr.ip6addr));
				break;

			case PW_VIRTUAL_SERVER:
				if (!request->server) return 0;

				snprintf(out, outlen, "%s", request->server);
				return strlen(out);
				break;

			case PW_MODULE_RETURN_CODE:
				localvp.da = da;
				
				/*
				 *	See modcall.c for a bit of a hack.
				 */
				localvp.vp_integer = request->simul_max;
				break;

			default:
				return 0; /* not found */
				break;
			}

			return valuepair2str(out, outlen, &localvp, da->type);
		}

		/*
		 *	Not found, die.
		 */
		return 0;
	}

	if (!vps) return 0;	/* silently fail */

	/*
	 *	Convert the VP to a string, and return it.
	 */
	return valuepair2str(out, outlen, vp, da->type);
}

/** Print data as integer, not as VALUE.
 *
 */
static size_t xlat_integer(UNUSED void *instance, REQUEST *request,
			   const char *fmt, char *out, size_t outlen)
{
	VALUE_PAIR 	*vp;

	uint64_t 	integer;
	
	while (isspace((int) *fmt)) fmt++;

	if ((radius_get_vp(request, fmt, &vp) < 0) || !vp) {
		*out = '\0';
		return 0;
	}

	switch (vp->da->type)
	{		
		case PW_TYPE_OCTETS:
		case PW_TYPE_STRING:
			if (vp->length > 8) {
				break;
			}

			memcpy(&integer, &(vp->vp_octets), vp->length);
			
			return snprintf(out, outlen, "%llu", ntohll(integer));	
			
		case PW_TYPE_INTEGER64:
			return snprintf(out, outlen, "%llu", vp->vp_integer64);
			
		case PW_TYPE_IPADDR:
		case PW_TYPE_INTEGER:
		case PW_TYPE_SHORT:
		case PW_TYPE_BYTE:
		case PW_TYPE_DATE:
			return snprintf(out, outlen, "%u", vp->vp_integer);
		default:
			break;
	}
	
	*out = '\0';
	return 0;
}

/** Print data as hex, not as VALUE.
 *
 */
static size_t xlat_hex(UNUSED void *instance, REQUEST *request,
		       const char *fmt, char *out, size_t outlen)
{
	size_t i;
	VALUE_PAIR *vp;
	uint8_t	buffer[MAX_STRING_LEN];
	ssize_t	ret;
	size_t	len;

	while (isspace((int) *fmt)) fmt++;

	if ((radius_get_vp(request, fmt, &vp) < 0) || !vp) {
		*out = '\0';
		return 0;
	}
	
	ret = rad_vp2data(vp, buffer, sizeof(buffer));
	len = (size_t) ret;
	
	/*
	 *	Don't truncate the data.
	 */
	if ((ret < 0 ) || (outlen < (len * 2))) {
		*out = 0;
		return 0;
	}

	for (i = 0; i < len; i++) {
		snprintf(out + 2*i, 3, "%02x", buffer[i]);
	}

	return len * 2;
}

/** Print data as base64, not as VALUE
 *
 */
static size_t xlat_base64(UNUSED void *instance, REQUEST *request,
			  const char *fmt, char *out, size_t outlen)
{
	VALUE_PAIR *vp;
	uint8_t buffer[MAX_STRING_LEN];
	ssize_t	ret;
	
	while (isspace((int) *fmt)) fmt++;

	if ((radius_get_vp(request, fmt, &vp) < 0) || !vp) {
		*out = '\0';
		return 0;
	}
	
	ret = rad_vp2data(vp, buffer, sizeof(buffer));
	if (ret < 0) {
		*out = 0;
		return 0;
	}

	return fr_base64_encode(buffer, (size_t) ret, out, outlen);
}

/** Prints the current module processing the request
 *
 */
static size_t xlat_module(UNUSED void *instance, REQUEST *request,
			  UNUSED const char *fmt, char *out, size_t outlen)
{
	strlcpy(out, request->module, outlen);

	return strlen(out);
}

#ifdef WITH_UNLANG
/** Implements the Foreach-Variable-X
 *
 * @see modcall()
 */
static size_t xlat_foreach(void *instance, REQUEST *request,
			   UNUSED const char *fmt, char *out, size_t outlen)
{
	VALUE_PAIR	**pvp;

	/*
	 *	See modcall, "FOREACH" for how this works.
	 */
	pvp = (VALUE_PAIR **) request_data_reference(request, radius_get_vp,
						     *(int*) instance);
	if (!pvp || !*pvp) {
		*out = '\0';
		return 0;
	}

	return valuepair2str(out, outlen, (*pvp), (*pvp)->da->type);
}
#endif

/** Print data as string, if possible.
 *
 * If attribute "Foo" is defined as "octets" it will normally
 * be printed as 0x0a0a0a. The xlat "%{string:Foo}" will instead
 * expand to "\n\n\n"
 */
static size_t xlat_string(UNUSED void *instance, REQUEST *request,
			  const char *fmt, char *out, size_t outlen)
{
	int len;
	VALUE_PAIR *vp;

	while (isspace((int) *fmt)) fmt++;

	if (outlen < 3) {
	nothing:
		*out = '\0';
		return 0;
	}

	if ((radius_get_vp(request, fmt, &vp) < 0) || !vp) goto nothing;

	if (vp->da->type != PW_TYPE_OCTETS) goto nothing;

	len = fr_print_string(vp->vp_strvalue, vp->length, out, outlen);
	out[len] = '\0';

	return len;
}

/** xlat expand string attribute value
 *
 */
static size_t xlat_xlat(UNUSED void *instance, REQUEST *request,
			const char *fmt, char *out, size_t outlen)
{
	VALUE_PAIR *vp;

	while (isspace((int) *fmt)) fmt++;

	if (outlen < 3) {
	nothing:
		*out = '\0';
		return 0;
	}

	if ((radius_get_vp(request, fmt, &vp) < 0) || !vp) goto nothing;

	return radius_xlat(out, outlen, request, vp->vp_strvalue, NULL, NULL);
}

#ifdef HAVE_REGEX_H
/** Expand regexp matches %{0} to %{8}
 *
 */
static size_t xlat_regex(void *instance, REQUEST *request,
			 UNUSED const char *fmt, char *out, size_t outlen)
{
	char *regex;

	/*
	 *	We cheat: fmt is "0" to "8", but those numbers
	 *	are already in the "instance".
	 */
	regex = request_data_reference(request, request,
				 REQUEST_DATA_REGEX | *(int *)instance);
	if (!regex) return 0;

	/*
	 *	Copy UP TO "freespace" bytes, including
	 *	a zero byte.
	 */
	strlcpy(out, regex, outlen);
	return strlen(out);
}
#endif				/* HAVE_REGEX_H */

/** Dynamically change the debugging level for the current request
 *
 * Example %{debug:3}
 */
static size_t xlat_debug(UNUSED void *instance, REQUEST *request,
			 const char *fmt, char *out, size_t outlen)
{
	int level = 0;
	
	/*
	 *  Expand to previous (or current) level
	 */
	snprintf(out, outlen, "%d", request->options & RAD_REQUEST_OPTION_DEBUG4);

	/*
	 *  Assume we just want to get the current value and NOT set it to 0
	 */
	if (!*fmt)
		goto done;
		
	level = atoi(fmt);
	if (level == 0) {
		request->options = RAD_REQUEST_OPTION_NONE;
		request->radlog = NULL;
	} else {
		if (level > 4) level = 4;

		request->options = level;
		request->radlog = radlog_request;
	}
	
	done:
	return strlen(out);
}

/*
 *	Compare two xlat_t structs, based ONLY on the module name.
 */
static int xlat_cmp(const void *a, const void *b)
{
	if (((const xlat_t *)a)->length != ((const xlat_t *)b)->length) {
		return ((const xlat_t *)a)->length - ((const xlat_t *)b)->length;
	}

	return memcmp(((const xlat_t *)a)->module,
		      ((const xlat_t *)b)->module,
		      ((const xlat_t *)a)->length);
}


/*
 *	find the appropriate registered xlat function.
 */
static xlat_t *xlat_find(const char *module)
{
	xlat_t my_xlat;

	strlcpy(my_xlat.module, module, sizeof(my_xlat.module));
	my_xlat.length = strlen(my_xlat.module);

	return rbtree_finddata(xlat_root, &my_xlat);
}


/** Register an xlat function.
 *
 * @param module xlat name
 * @param func xlat function to be called
 * @param instance argument to xlat function
 * @return 0 on success, -1 on failure
 */
int xlat_register(const char *module, RAD_XLAT_FUNC func, void *instance)
{
	xlat_t	*c;
	xlat_t	my_xlat;

	if (!module || !*module) {
		DEBUG("xlat_register: Invalid module name");
		return -1;
	}

	/*
	 *	First time around, build up the tree...
	 *
	 *	FIXME: This code should be hoisted out of this function,
	 *	and into a global "initialization".  But it isn't critical...
	 */
	if (!xlat_root) {
		int i;
#ifdef HAVE_REGEX_H
		char buffer[2];
#endif

		xlat_root = rbtree_create(xlat_cmp, free, 0);
		if (!xlat_root) {
			DEBUG("xlat_register: Failed to create tree.");
			return -1;
		}

		/*
		 *	Register the internal packet xlat's.
		 */
		for (i = 0; internal_xlat[i] != NULL; i++) {
			xlat_register(internal_xlat[i], xlat_packet, &xlat_inst[i]);
			c = xlat_find(internal_xlat[i]);
			rad_assert(c != NULL);
			c->internal = TRUE;
		}

#ifdef WITH_UNLANG
		for (i = 0; xlat_foreach_names[i] != NULL; i++) {
			xlat_register(xlat_foreach_names[i],
				      xlat_foreach, &xlat_inst[i]);
			c = xlat_find(xlat_foreach_names[i]);
			rad_assert(c != NULL);
			c->internal = TRUE;
		}
#endif

		/*
		 *	New name: "control"
		 */
		xlat_register("control", xlat_packet, &xlat_inst[0]);
		c = xlat_find("control");
		rad_assert(c != NULL);
		c->internal = TRUE;

#define XLAT_REGISTER(_x) xlat_register(Stringify(_x), xlat_ ## _x, NULL); \
		c = xlat_find(Stringify(_x)); \
		rad_assert(c != NULL); \
		c->internal = TRUE

		XLAT_REGISTER(integer);
		XLAT_REGISTER(hex);
		XLAT_REGISTER(base64);
		XLAT_REGISTER(string);
		XLAT_REGISTER(xlat);
		XLAT_REGISTER(module);

#ifdef HAVE_REGEX_H
		/*
		 *	Register xlat's for regexes.
		 */
		buffer[1] = '\0';
		for (i = 0; i <= REQUEST_MAX_REGEX; i++) {
			buffer[0] = '0' + i;
			xlat_register(buffer, xlat_regex, &xlat_inst[i]);
			c = xlat_find(buffer);
			rad_assert(c != NULL);
			c->internal = TRUE;
		}
#endif /* HAVE_REGEX_H */


		xlat_register("debug", xlat_debug, &xlat_inst[0]);
		c = xlat_find("debug");
		rad_assert(c != NULL);
		c->internal = TRUE;
	}

	/*
	 *	If it already exists, replace the instance.
	 */
	strlcpy(my_xlat.module, module, sizeof(my_xlat.module));
	my_xlat.length = strlen(my_xlat.module);
	c = rbtree_finddata(xlat_root, &my_xlat);
	if (c) {
		if (c->internal) {
			DEBUG("xlat_register: Cannot re-define internal xlat");
			return -1;
		}

		c->do_xlat = func;
		c->instance = instance;
		return 0;
	}

	/*
	 *	Doesn't exist.  Create it.
	 */
	c = rad_malloc(sizeof(*c));
	memset(c, 0, sizeof(*c));

	c->do_xlat = func;
	strlcpy(c->module, module, sizeof(c->module));
	c->length = strlen(c->module);
	c->instance = instance;

	rbtree_insert(xlat_root, c);

	return 0;
}

/** Unregister an xlat function
 *
 * We can only have one function to call per name, so the passing of "func"
 * here is extraneous.
 *
 * @param[in] module xlat to unregister.
 * @param[in] func
 * @param[in] instance
 */
void xlat_unregister(const char *module, UNUSED RAD_XLAT_FUNC func, void *instance)
{
	xlat_t	*c;
	xlat_t		my_xlat;

	if (!module) return;

	strlcpy(my_xlat.module, module, sizeof(my_xlat.module));
	my_xlat.length = strlen(my_xlat.module);

	c = rbtree_finddata(xlat_root, &my_xlat);
	if (!c) return;

	if (c->instance != instance) return;

	rbtree_deletebydata(xlat_root, c);
}

/** De-register all xlat functions, used mainly for debugging.
 *
 */
void xlat_free(void)
{
	rbtree_free(xlat_root);
}


/** Decode an attribute name into a string
 *
 * This expands the various formats:
 * - %{Name}
 * - %{xlat:name}
 * - %{Name:-Other}
 *
 * Calls radius_xlat() to do most of the work.
 *
 * @param[in] from string to expand.
 * @param[in,out] to buffer for output.
 * @param[in] freespace remaining in output buffer.
 * @param[in] request Current server request.
 * @param[in] func Optional function to escape output; passed to radius_xlat().
 * @param[in] funcarg pointer to pass to escape function.
 * @return 0 on success, -1 on failure.
 */
static ssize_t decode_attribute(const char **from, char **to, int freespace, REQUEST *request,
				RADIUS_ESCAPE_STRING func, void *funcarg)
{
	int	do_length = 0;
	const char *module_name, *xlat_str;
	char *p, *q, *l, *next = NULL;
	int retlen=0;
	const xlat_t *c;
	int varlen;
	char buffer[8192];

	q = *to;

	*q = '\0';

	/*
	 *	Copy the input string to an intermediate buffer where
	 *	we can mangle it.
	 */
	varlen = rad_copy_variable(buffer, *from);
	if (varlen < 0) {
		RDEBUG2E("Badly formatted variable: %s", *from);
		return -1;
	}
	*from += varlen;

	/*
	 *	Kill the %{} around the data we are looking for.
	 */
	p = buffer;
	p[varlen - 1] = '\0';	/*  */
	p += 2;
	if (*p == '#') {
		p++;
		do_length = 1;
	}

	/*
	 *	Handle %{%{foo}:-%{bar}}, which is useful, too.
	 *
	 *	Did I mention that this parser is garbage?
	 */
	if ((p[0] == '%') && (p[1] == '{')) {
		int len1, len2;
		int expand2 = FALSE;

		/*
		 *	'p' is after the start of 'buffer', so we can
		 *	safely do this.
		 */
		len1 = rad_copy_variable(buffer, p);
		if (len1 < 0) {
			RDEBUG2E("Badly formatted variable: %s", p);
			return -1;
		}

		/*
		 *	They did %{%{foo}}, which is stupid, but allowed.
		 */
		if (!p[len1]) {
			RDEBUG2("Improperly nested variable; %%{%s}", p);
			return -1;
		}

		/*
		 *	It SHOULD be %{%{foo}:-%{bar}}.  If not, it's
		 *	an error.
		 */
		if ((p[len1] != ':') || (p[len1 + 1] != '-')) {
			RDEBUG2("No trailing :- after variable at %s", p);
			return -1;
		}

		/*
		 *	Parse the second bit.  The second bit can be
		 *	either %{foo}, or a string "foo", or a string
		 *	'foo', or just a bare word: foo
		 */
		p += len1 + 2;
		l = buffer + len1 + 1;

		if ((p[0] == '%') && (p[1] == '{')) {
			len2 = rad_copy_variable(l, p);

			if (len2 < 0) {
				RDEBUG2E("Invalid text after :- at %s", p);
				return -1;
			}
			p += len2;
			expand2 = TRUE;

		} else if ((p[0] == '"') || p[0] == '\'') {
		  getstring((const char **) &p, l, strlen(l));

		} else {
			l = p;
		}

		/*
		 *	Expand the first one.  If we did, exit the
		 *	conditional.
		 */
		retlen = radius_xlat(q, freespace, request, buffer, func, funcarg);
		if (retlen < 0) {
			return retlen;
		}
		
		if (retlen) {
			q += retlen;
			goto done;
		}

		RDEBUG2("\t... expanding second conditional");
		/*
		 *	Expand / copy the second string if required.
		 */
		if (expand2) {
			retlen = radius_xlat(q, freespace, request, l, func, funcarg);
			if (retlen < 0) {
				return retlen;
			}
			
			if (retlen) {
				q += retlen;
			}
		} else {
			strlcpy(q, l, freespace);
			q += strlen(q);
		}

		/*
		 *	Else the output is an empty string.
		 */
		goto done;
	}

	/*
	 *	See if we're supposed to expand a module name.
	 */
	module_name = NULL;
	for (l = p; *l != '\0'; l++) {
		/*
		 *	module:string
		 */
		if (*l == ':') {
			module_name = p; /* start of name */
			*l = '\0';
			p = l + 1;
			break;
		}

		/*
		 *	Module names can't have spaces.
		 */
		if ((*l == ' ') || (*l == '\t')) break;
	}

	/*
	 *	%{name} is a simple attribute reference,
	 *	or regex reference.
	 */
	if (!module_name) {
		if (isdigit(*p) && !p[1]) { /* regex 0..8 */
			module_name = xlat_str = p;
		} else {
			xlat_str = p;
		}
		goto do_xlat;
	}

	/*
	 *	Maybe it's the old-style %{foo:-bar}
	 */
	if (*p == '-') {
		RDEBUG2W("Deprecated conditional expansion \":-\".  See \"man unlang\" for details");
		p++;

		xlat_str = module_name;
		next = p;
		goto do_xlat;
	}

	/*
	 *	FIXME: For backwards "WTF" compatibility, check for
	 *	{...}, (after the :), and copy that, too.
	 */

	/* module name, followed by (possibly) per-module string */
	xlat_str = p;
	
do_xlat:
	/*
	 *	Just "foo".  Maybe it's a magic attr, which doesn't
	 *	really exist.
	 *
	 *	If we can't find that, then assume it's a dictionary
	 *	attribute in the request.
	 *
	 *	Else if it's module:foo, look for module, and pass it "foo".
	 */
	if (!module_name) {
		c = xlat_find(xlat_str);
		if (!c) c = xlat_find("request");
	} else {
		c = xlat_find(module_name);
	}
	if (!c) {
		if (!module_name) {
			RDEBUG2W("Unknown Attribute \"%s\" in string expansion \"%%%s\"", xlat_str, *from);
		} else {
			RDEBUG2W("Unknown module \"%s\" in string expansion \"%%%s\"", module_name, *from);
		}
		return -1;
	}

	if (!c->internal) {
		RDEBUG3("Running registered xlat function of module %s for string \'%s\'",
			c->module, xlat_str);
	}
	if (func) {
		/* xlat to a temporary buffer, then escape */
		char tmpbuf[8192];

		retlen = c->do_xlat(c->instance, request, xlat_str, tmpbuf, sizeof(tmpbuf));
		if (retlen > 0) {
			retlen = func(request, q, freespace, tmpbuf, funcarg);
			if (retlen > 0) {
				RDEBUG2("\tescape: \'%s\' -> \'%s\'", tmpbuf, q);
			} else if (retlen < 0) {
				RDEBUG2("String escape failed");
			}
		}
	} else {
		retlen = c->do_xlat(c->instance, request, xlat_str, q, freespace);
	}
	if (retlen > 0) {
		if (do_length) {
			snprintf(q, freespace, "%d", retlen);
			retlen = strlen(q);
		}
		
	} else if (next) {
		/*
		 *	Expand the second bit.
		 */
		RDEBUG2("\t... expanding second conditional");
		retlen = radius_xlat(q, freespace, request, next, func, funcarg);
		if (retlen < 0) {
			return retlen;
		}
	}
	q += retlen;

done:
	*to = q;
	return 0;
}

/** Chop the tokens string
 *
 * Insert null delimiters into the tokens string, to form tokens, and set the node fmt pointer
 * to be the start of the token.
 *
 * @param p start of the token.
 * @param q end of the token (will be replace with '\q').
 * @param node to set the fmt string in.
 * @return q + 1
 */
inline static char * radius_xlat_chop(char *p, char *q, xlat_exp_t *node)
{
	/* 
	 *	Terminate current token
	 */
	*q = '\0';

	node->fmt = p;
	node->len = q - p;

	/*
	 *	Return the start of the next token
	 */
	return q + 1;
}

/** Convert an xlat expression into a tree
 *
 * @param fmt string.
 * @param tokens Mutable copy of fmt string, should be a talloc child of the root node.
 * @param def Default xlat to use for expansions which do not formerly specify an xlat function.
 * @param node to add to children, alternates or siblings to.
 * @return On success, the number of bytes of fmt processed, on error the position of the error * -1.
 */
static ssize_t radius_xlat_tokenize_r(const char *fmt, char *tokens, size_t len, const xlat_t *def, xlat_exp_t *node)
{
	size_t i;
	xlat_exp_t *head = node;
	
	char *p = tokens;
	uint8_t c;
	
	ssize_t slen;
	xlat_state_t state = XLAT_LITERAL;
	const xlat_t *xlat;
	
	rad_assert(node);

	for (i = 0; i < len; i++) {
		switch (state) {
		/*
		 *	"<we are here> %{}"
		 *
		 *	or
		 *
		 *	"for bar baz %{%{func:<here>}}"
		 *
		 *	or
		 *
		 *	"foo bar baz %{%{}:-<here>}"
		 *
		 */
		case XLAT_LITERAL:
			literal:
			if (fmt[i] == '\\') switch (fmt[i + 1]) {
				case '\\':
					head = head->next = talloc_zero(head, xlat_exp_t);
					p = radius_xlat_chop(p, tokens + (i + 1), head);
			
					i++;
					continue;
			
				case 't':
					tokens[i] = '\t';

					i++;
					continue;
			
				case 'n':
					tokens[i] = '\n';
			
					i++;
					continue;
			
				case 'x':
					{
					/* We expect two digits [0-9] */
					if ((len - i) < 3) {
						return -1;
					}
			
					if (!fr_hex2bin(tokens + i, &c, 1)) {
						return -1;
					}
			
					tokens[i] = (char) c;
				
					head = head->next = talloc_zero(head, xlat_exp_t);
					p = radius_xlat_chop(p, tokens + (i + 1), head);
			
					i += 2;
					continue;
					}
			
				default:
					i++;
					continue;	
			}
		
			/*
			 *	We found the beginning of an expansion '%{'
			 */
			if ((fmt[i] == '%') && (fmt[i + 1] == '{')) {
				/*
				 *	We only need to do add a node here if we were previously processing
				 *	a literal string.
				 */
				if (i) {
					head = head->next = talloc_zero(head, xlat_exp_t);
					p = radius_xlat_chop(p, tokens + i, head);
				}			
			
				i += 2;	/* Next iteration we process the interior %{ */
				p = tokens + i;

				/*
				 *	This isn't a function or attribute, it's a container.
				 */
				if ((fmt[i] == '%') && (fmt[i + 1] == '{')) {
					state = XLAT_ALTERNATE;
					goto alternate;
				}
				
				state = XLAT_EXPANSION;
				goto expansion;
			}

			/*
			 *	We found a '}' this could be a formatting error, or it could be
			 *	we were parsing the arguments to an xlat function or alternate.
			 *
			 *	e.g. "%{func:<this}>" or %{%{}:-<this}> 
			 *
			 */
			if ((fmt[i] == '}') || ((fmt[i] == ':') && (fmt[i + 1] == '-'))) {
				/* Any more trailing literals? */
				if (p != (tokens + i)) {
					head = head->next = talloc_zero(head, xlat_exp_t);
					p = radius_xlat_chop(p, tokens + i, head);	
				}

				return i;
			}
			
			break;
			
		case XLAT_ALTERNATE:
			alternate:
			/*
			 *	We found a '%{%{', earlier, recurse to deal with the interior expression
			 */
			head = head->next = talloc_zero(head, xlat_exp_t);
			slen = radius_xlat_tokenize_r(fmt + i, tokens + i, len - i, def, head);
			if (slen < 0) {
				return slen - i;	/* correct error offset */
			}
			if (slen == 0) {
				DEBUGE("Invalid alternation: Zero length left operand");
				
				return (i - 1) * -1; 
			}
			
			/* Fixup the tree structure */
			head->child = head->next;
			head->next = NULL;
		
			i += slen;
			
			/*
			 *	We should now be "foo bar baz %{%{}<here>}", which is the only place
			 *	an alternate operator can legally appear.
			 */
			if ((fmt[i] == ':') && (fmt[i + 1] == '-')) {				
				i += 2;

				slen = radius_xlat_tokenize_r(fmt + i, tokens + i, len - i, def, head);
				if (slen < 0) {
					return slen - i;	/* correct error offset */
				}		
				if (slen == 0) {
					DEBUGE("Invalid alternation: Zero length right operand");
					
					return (i - 1) * -1; 
				}
				
				/* Fixup the tree structure */
				head->alternate = head->next;
				head->next = NULL;
				
				i += slen;
			}

			/*
			 *	End of expansion '}'
			 *
			 */
			if (fmt[i] == '}') {
				i++;
				p = tokens + i;
				
				state = XLAT_LITERAL;
				goto literal;
			}
			
			DEBUGE("Invalid alternation: Expected closing brace");
			return i * -1;

		/*
		 *	"foo bar baz %{<we are here>}" (and we know it's an expansion)
		 *
		 *	Now we need to figure out what type...
		 */				
		case XLAT_EXPANSION:
			expansion:
			/*
			 *	The square brackets are a slightly hacky addition for attribute selectors
			 */
			if (isalnum(fmt[i]) || (fmt[i] == '-') || (fmt[i] == '_') ||
			    (fmt[i] == '[') || (fmt[i] == ']') || (fmt[i] == '.')) {
				continue;
			
			/*
			 *	Function specifier "foo bar baz %{func:"
			 *
			 *	Recurse to deal with the argument string (which starts off being treated
			 *	as a literal).
			 */
			} else if (fmt[i] == ':') {
				/*
				 *	This is a common typo because of the legacy expression format
				 */
				if (fmt[i + 1] == '-') {
					DEBUGE("Invalid alternation: Left operand for ':-' must be an '%%{expansion}'");
				
					return (p - tokens) * -1; 
				}

				/*
				 *	Resolve the xlat function
				 */
				tokens[i] = '\0';
				xlat = xlat_find(p);
			
				if (!xlat) {
					DEBUGE("Invalid expansion: No such function \"%s\"", p);
				
					return (p - tokens) * -1;
				}
			
				i++;
				
				head = head->next = talloc_zero(head, xlat_exp_t);
				head->xlat = xlat;
				slen = radius_xlat_tokenize_r(fmt + i, tokens + i, len - i, def, head);
				if (slen < 0) {
					return slen - i;	/* correct error offset */
				}
				if (slen == 0) {
					DEBUGE("Invalid expansion: zero length argument string");
					
					return (i - 1) * -1; 
				}
				
				/* Correct the tree structure */
				head->child = head->next;
				head->next = NULL;
				
				i += slen;
			
				/*
				 *	End of expansion '}'
				 */
				if (fmt[i] != '}') {
					DEBUGE("Invalid expansion: Missing terminating '}'");
				
					return i * -1;
				}
				
				/*
				 *	Start of next literal/expansion/alternation
				 */
				i++;
				p = tokens + i;
				
				state = XLAT_LITERAL;
				goto literal;
			/*
			 *	This emulates normal list section - it's an unqualified attribute %{attribute}
			 */
			} else if (fmt[i] == '}') {
				if (!def) {
					DEBUGE("No default xlat function provided");
					
					return (p - tokens) * -1; 
				}
				
				/*
				 *	Pretend we got %{def:<str>}
				 */
				head = head->next = talloc_zero(head, xlat_exp_t);
				head->xlat = def;
				
				head->child = talloc_zero(head, xlat_exp_t);
				p = radius_xlat_chop(p, tokens + i, head->child);
				
				state = XLAT_LITERAL;
				goto literal;
			}
		
			DEBUGE("Invalid expansion: char '%c' is not allowed in xlat function or attribute names",
			       tokens[i]);
		
			return i * -1;
		}
	}
	
	/*
	 *	We reached the end of the format string, if there are trailing literals add them now
	 */
	if (p != (tokens + i)) {
		head = head->next = talloc_zero(head, xlat_exp_t);
		p = radius_xlat_chop(p, tokens + i, head);	
	}
	
	return i;
}

static void radius_xlat_tokenize_debug(xlat_exp_t *node, int lvl)
{
	char *pad = malloc(lvl);
	memset(pad, '\t', lvl);
	pad[lvl] = '\0';
	
	while(node) {
		if (node->fmt) {
			DEBUG("%sliteral \"%s\"", pad, node->fmt);
		}
		if (node->xlat) {
			DEBUG("%sxlat \"%s\"", pad, node->xlat->module);
		}
		if (!node->fmt && !node->xlat && !node->child && !node->alternate) {
			DEBUG("%sempty", pad);
		}
	
		if (node->child) {
			DEBUG("%s{", pad);
			radius_xlat_tokenize_debug(node->child, lvl + 1);
			DEBUG("%s}", pad);
		}
	
		if (node->alternate) {
			DEBUG("%selse {", pad);
			radius_xlat_tokenize_debug(node->alternate, lvl + 1);
			DEBUG("%s}", pad);
		}
		
		node = node->next;
	};
	
	free(pad);
}
static ssize_t radius_xlat_tokenize(TALLOC_CTX *ctx, const char *fmt, xlat_exp_t **node)
{
	ssize_t len;
	ssize_t inlen;
	const xlat_t *def;
	/* 
	 *	Copy the original format string to a buffer so we can mangle it.
	 */
	char *tokens;
	
	def = xlat_find("request");
	rad_assert(def);
	
	tokens = talloc_strdup(ctx, fmt);

	/*
	 *	Allocate an empty root node
	 */	
	*node = talloc_zero(tokens, xlat_exp_t);
	
	inlen = talloc_get_size(tokens) - 1;
	
	len = radius_xlat_tokenize_r(fmt, tokens, inlen, def, *node);
	/*
	 *	Output something like:
	 *
	 *	"format string"
	 *	"       ^ error was here"
	 */
	if (len < 0) {
		size_t pos = (size_t) len * -1;
		char *pad = malloc(pos);
		memset(pad, ' ', pos);
		
		pad[len * -1] = '\0';
		
		DEBUGE("%s", fmt);
		DEBUGE("%s^ error was here", pad);
		
		free(pad);
		
		talloc_free(*node);
		*node = NULL;
		
		return len;
	}
	
	if (len != inlen) {
		DEBUGE("Too many '}'");

		talloc_free(*node);
		*node = NULL;
		
		return len * -1;
	}

#if 0
	DEBUG("%s", fmt);
	DEBUG("Parsed xlat tree:");
	radius_xlat_tokenize_debug(*node, 0);
#endif

	return len;
}

/** Replace %whatever in a string.
 *
 * See 'doc/variables.txt' for more information.
 *
 * @param[out] out Where to write pointer to output buffer.
 * @param[in] ctx Where to write the pointer to the output buffer.
 * @param[in] request current request.
 * @param[in] fmt string to expand.
 * @param[in] func function to escape final value e.g. SQL quoting.
 * @param[in] ctx pointer to pass to escape function.
 * @return length of string written @bug should really have -1 for failure
 */    	   
static ssize_t xlat_expand(char **out, size_t outlen, REQUEST *request, const char *fmt, RADIUS_ESCAPE_STRING escape,
		    	   void *ctx)
{
	char *buff;
	
	int c, freespace;
	const char *p;
	char *q;
	char *nl;
	VALUE_PAIR *tmp;
	struct tm *TM, s_TM;
	char tmpdt[40]; /* For temporary storing of dates */
	ssize_t len, bufflen;
	
	rad_assert(fmt);
	rad_assert(request);
	
	xlat_exp_t *node;
	
	/* Temporary */
	radius_xlat_tokenize(request, fmt, &node);
	talloc_free(node);
	
	/*
	 *	Caller either needs to pass us a NULL buffer and a 0 length size, or a non-NULL buffer
	 *	and the size of that buffer.
	 */
	if (!*out) {
		rad_assert(outlen == 0);
	
		*out = buff = talloc_array(request, char, 8192);
		bufflen = 8192;
	} else {
		rad_assert(outlen != 0);
	
		buff = *out;
		bufflen = outlen;
	}

       	q = buff;
	p = fmt;
	while (*p) {
		/* Calculate freespace in output */
		freespace = bufflen - (q - buff);
		if (freespace <= 1)
			break;
		c = *p;

		if ((c != '%') && (c != '$') && (c != '\\')) {
			/*
			 * We check if we're inside an open brace.  If we are
			 * then we assume this brace is NOT literal, but is
			 * a closing brace and apply it
			 */
			*q++ = *p++;
			continue;
		}

		/*
		 *	There's nothing after this character, copy
		 *	the last '%' or "$' or '\\' over to the output
		 *	buffer, and exit.
		 */
		if (*++p == '\0') {
			*q++ = c;
			break;
		}

		if (c == '\\') {
			switch(*p) {
			case '\\':
				*q++ = *p;
				break;
			case 't':
				*q++ = '\t';
				break;
			case 'n':
				*q++ = '\n';
				break;
			default:
				*q++ = c;
				*q++ = *p;
				break;
			}
			p++;

		} else if (c == '%') switch(*p) {
			case '{':
				p--;
				len = decode_attribute(&p, &q, freespace, request, escape, ctx);
				if (len < 0) {
					return len;
				}
				
				break;

			case '%':
				*q++ = *p++;
				break;
			case 'd': /* request day */
				TM = localtime_r(&request->timestamp, &s_TM);
				len = strftime(tmpdt, sizeof(tmpdt), "%d", TM);
				if (len > 0) {
					strlcpy(q, tmpdt, freespace);
					q += strlen(q);
				}
				p++;
				break;
			case 'l': /* request timestamp */
				snprintf(tmpdt, sizeof(tmpdt), "%lu",
					 (unsigned long) request->timestamp);
				strlcpy(q,tmpdt,freespace);
				q += strlen(q);
				p++;
				break;
			case 'm': /* request month */
				TM = localtime_r(&request->timestamp, &s_TM);
				len = strftime(tmpdt, sizeof(tmpdt), "%m", TM);
				if (len > 0) {
					strlcpy(q, tmpdt, freespace);
					q += strlen(q);
				}
				p++;
				break;
			case 't': /* request timestamp */
				CTIME_R(&request->timestamp, tmpdt, sizeof(tmpdt));
				nl = strchr(tmpdt, '\n');
				if (nl) *nl = '\0';
				strlcpy(q, tmpdt, freespace);
				q += strlen(q);
				p++;
				break;
			case 'C': /* ClientName */
				strlcpy(q,request->client->shortname,freespace);
				q += strlen(q);
				p++;
				break;
			case 'D': /* request date */
				TM = localtime_r(&request->timestamp, &s_TM);
				len = strftime(tmpdt, sizeof(tmpdt), "%Y%m%d", TM);
				if (len > 0) {
					strlcpy(q, tmpdt, freespace);
					q += strlen(q);
				}
				p++;
				break;
			case 'G': /* request minute */
				TM = localtime_r(&request->timestamp, &s_TM);
				len = strftime(tmpdt, sizeof(tmpdt), "%M", TM);
				if (len > 0) {
					strlcpy(q, tmpdt, freespace);
					q += strlen(q);
				}
				p++;
				break;
			case 'H': /* request hour */
				TM = localtime_r(&request->timestamp, &s_TM);
				len = strftime(tmpdt, sizeof(tmpdt), "%H", TM);
				if (len > 0) {
					strlcpy(q, tmpdt, freespace);
					q += strlen(q);
				}
				p++;
				break;
			case 'I': /* Request ID */
				snprintf(tmpdt, sizeof(tmpdt), "%i", request->packet->id);
				strlcpy(q, tmpdt, freespace);
				q += strlen(q);
				p++;
				break;
			case 'S': /* request timestamp in SQL format*/
				TM = localtime_r(&request->timestamp, &s_TM);
				len = strftime(tmpdt, sizeof(tmpdt), "%Y-%m-%d %H:%M:%S", TM);
				if (len > 0) {
					strlcpy(q, tmpdt, freespace);
					q += strlen(q);
				}
				p++;
				break;
			case 'T': /* request timestamp */
				TM = localtime_r(&request->timestamp, &s_TM);
				len = strftime(tmpdt, sizeof(tmpdt), "%Y-%m-%d-%H.%M.%S.000000", TM);
				if (len > 0) {
					strlcpy(q, tmpdt, freespace);
					q += strlen(q);
				}
				p++;
				break;
			case 'V': /* Request-Authenticator */
				strlcpy(q,"Verified",freespace);
				q += strlen(q);
				p++;
				break;
			case 'Y': /* request year */
				TM = localtime_r(&request->timestamp, &s_TM);
				len = strftime(tmpdt, sizeof(tmpdt), "%Y", TM);
				if (len > 0) {
					strlcpy(q, tmpdt, freespace);
					q += strlen(q);
				}
				p++;
				break;
			case 'Z': /* Full request pairs except password */
				tmp = request->packet->vps;
				while (tmp && (freespace > 3)) {
					if (!(!tmp->da->vendor &&
					    (tmp->da->attr == PW_USER_PASSWORD))) {
						*q++ = '\t';
						len = vp_prints(q, freespace - 2, tmp);
						q += len;
						freespace -= (len + 2);
						*q++ = '\n';
					}
					tmp = tmp->next;
				}
				p++;
				break;
			default:
				RDEBUG2W("Unknown variable '%%%c': See 'doc/variables.txt'", *p);
				if (freespace > 2) {
					*q++ = '%';
					*q++ = *p++;
				}
				
				goto error;
		}
	}
	*q = '\0';

	RDEBUG2("\texpand: '%s' -> '%s'", fmt, buff);

	return strlen(buff);
	
	error:
	talloc_free(*out);
	*out = NULL;
	
	return -1;
}

ssize_t radius_xlat(char *out, size_t outlen, REQUEST *request, const char *fmt, RADIUS_ESCAPE_STRING escape, void *ctx)
{
	return xlat_expand(&out, outlen, request, fmt, escape, ctx);
}
		    
ssize_t radius_axlat(char **out, REQUEST *request, const char *fmt, RADIUS_ESCAPE_STRING escape, void *ctx)
{
	return xlat_expand(out, 0, request, fmt, escape, ctx);
}
