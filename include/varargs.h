/*
 *  Copyright (C) 2004 Jack O'Quin
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#ifndef __jack_varargs_h__
#define __jack_varargs_h__

#ifdef __cplusplus
extern "C" {
#endif

/* variable argument structure */
typedef struct {
	char *server_name;              /* server name */
	char *load_name;                /* load module name */
	char *load_init;                /* initialization string */
	char *sess_uuid;
} jack_varargs_t;

static inline void
jack_varargs_init (jack_varargs_t *va)
{
	memset (va, 0, sizeof(jack_varargs_t));
	va->server_name = jack_default_server_name ();
}

static inline void
jack_varargs_parse (jack_options_t options, va_list ap, jack_varargs_t *va)
{
	/* initialize default settings */
	jack_varargs_init (va);

	if ((options & JackServerName)) {
		char *sn = va_arg (ap, char *);
		if (sn) {
			va->server_name = sn;
		}
	}
	if ((options & JackLoadName)) {
		va->load_name = va_arg (ap, char *);
	}
	if ((options & JackLoadInit)) {
		va->load_init = va_arg (ap, char *);
	}
	if ((options & JackSessionID)) {
		va->sess_uuid = va_arg (ap, char *);
	}
}

#ifdef __cplusplus
}
#endif

#endif /* __jack_varargs_h__ */
