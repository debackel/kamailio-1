/*
 * $Id$
 *
 * Copyright (C) 2001-2003 FhG Fokus
 * Copyright (C) 2011 Carsten Bock, carsten@ng-voice.com
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*!
 * \file
 * \brief Route & Record-Route module
 * \ingroup rr
 */


#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <regex.h>

#include "../../sr_module.h"
#include "../../ut.h"
#include "../../error.h"
#include "../../pvar.h"
#include "../../mem/mem.h"
#include "../../mod_fix.h"
#include "../../parser/parse_rr.h"
#include "../outbound/api.h"
#include "loose.h"
#include "record.h"
#include "rr_cb.h"
#include "api.h"

#ifdef ENABLE_USER_CHECK
#include <string.h>
#include "../../str.h"
str i_user;
char *ignore_user = NULL;
#endif

int append_fromtag = 1;		/*!< append from tag by default */
int enable_double_rr = 1;	/*!< enable using of 2 RR by default */
int enable_full_lr = 0;		/*!< compatibilty mode disabled by default */
int add_username = 0;	 	/*!< do not add username by default */
int enable_socket_mismatch_warning = 1; /*!< enable socket mismatch warning */
static str custom_user_spec = {NULL, 0};
pv_spec_t custom_user_avp;

static unsigned int last_rr_msg;
ob_api_t rr_obb;

MODULE_VERSION

static int  mod_init(void);static void mod_destroy(void);
/* fixup functions */
static int direction_fixup(void** param, int param_no);
static int it_list_fixup(void** param, int param_no);
/* wrapper functions */
static int w_loose_route(struct sip_msg *, char *, char *);
static int w_record_route(struct sip_msg *, char *, char *);
static int w_record_route_preset(struct sip_msg *,char *, char *);
static int w_record_route_advertised_address(struct sip_msg *, char *, char *);
static int w_add_rr_param(struct sip_msg *,char *, char *);
static int w_check_route_param(struct sip_msg *,char *, char *);
static int w_is_direction(struct sip_msg *,char *, char *);
/* PV functions */
static int pv_get_route_uri_f(struct sip_msg *, pv_param_t *, pv_value_t *);
/*!
 * \brief Exported functions
 */
static cmd_export_t cmds[] = {
	{"loose_route",          (cmd_function)w_loose_route,		0, 0, 0,
			REQUEST_ROUTE},
	{"record_route",         (cmd_function)w_record_route,		0, 0, 0,
			REQUEST_ROUTE|BRANCH_ROUTE|FAILURE_ROUTE},
	{"record_route",         (cmd_function)w_record_route, 		1, it_list_fixup, 0,
			REQUEST_ROUTE|BRANCH_ROUTE|FAILURE_ROUTE},
	{"record_route_preset",  (cmd_function)w_record_route_preset, 1, it_list_fixup, 0,
			REQUEST_ROUTE|BRANCH_ROUTE|FAILURE_ROUTE},
	{"record_route_preset",  (cmd_function)w_record_route_preset, 2, it_list_fixup, 0,
			REQUEST_ROUTE|BRANCH_ROUTE|FAILURE_ROUTE},
	{"record_route_advertised_address",  (cmd_function)w_record_route_advertised_address, 1, it_list_fixup, 0,
			REQUEST_ROUTE|BRANCH_ROUTE|FAILURE_ROUTE},
	{"add_rr_param",         (cmd_function)w_add_rr_param,	1, it_list_fixup, 0,
			REQUEST_ROUTE|BRANCH_ROUTE|FAILURE_ROUTE},
	{"check_route_param",    (cmd_function)w_check_route_param, 1, fixup_regexp_null, fixup_free_regexp_null,
			REQUEST_ROUTE},
	{"is_direction",         (cmd_function)w_is_direction, 		1, direction_fixup, 0,
			REQUEST_ROUTE},
	{"load_rr",              (cmd_function)load_rr, 				0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0}
};


/*!
 * \brief Exported parameters
 */
static param_export_t params[] ={ 
	{"append_fromtag",	INT_PARAM, &append_fromtag},
	{"enable_double_rr",	INT_PARAM, &enable_double_rr},
	{"enable_full_lr",		INT_PARAM, &enable_full_lr},
#ifdef ENABLE_USER_CHECK
	{"ignore_user",		STR_PARAM, &ignore_user},
#endif
	{"add_username",		INT_PARAM, &add_username},
	{"enable_socket_mismatch_warning",INT_PARAM,&enable_socket_mismatch_warning},
	{"custom_user_avp",           STR_PARAM, &custom_user_spec.s},
	{0, 0, 0 }
};

/*!
 * \brief Exported Pseudo variables
 */
static pv_export_t mod_pvs[] = {
    {{"route_uri", (sizeof("route_uri")-1)}, /* URI of the first Route-Header */
     PVT_OTHER, pv_get_route_uri_f, 0, 0, 0, 0, 0},
    {{0, 0}, 0, 0, 0, 0, 0, 0, 0}
};



struct module_exports exports = {
	"rr",
	DEFAULT_DLFLAGS,	/*!< dlopen flags */
	cmds,			/*!< Exported functions */
	params,			/*!< Exported parameters */
	0,				/*!< exported statistics */
	0,				/*!< exported MI functions */
	mod_pvs,			/*!< exported pseudo-variables */
	0,				/*!< extra processes */
	mod_init,			/*!< initialize module */
	0,				/*!< response function*/
	mod_destroy,		/*!< destroy function */
	0				/*!< per-child init function */
};


static int mod_init(void)
{
	if (ob_load_api(&rr_obb) == 0)
		LM_INFO("Bound rr module to outbound module\n");
	else
	{
		LM_INFO("outbound module not available\n");
		memset(&rr_obb, 0, sizeof(ob_api_t));
	}

#ifdef ENABLE_USER_CHECK
	if(ignore_user)
	{
		if (rr_obb.use_outbound)
		{
			LM_ERR("cannot use \"ignore_user\" with outbound\n");
			return -1;
		}
		i_user.s = ignore_user;
		i_user.len = strlen(ignore_user);
	}
	else
	{
		i_user.s = 0;
		i_user.len = 0;
	}
#endif

	if (add_username != 0 && rr_obb.use_outbound)
	{
		LM_ERR("cannot use \"add_username\" with outbound\n");
		return -1;
	}

	if (custom_user_spec.s) {
		custom_user_spec.len = strlen(custom_user_spec.s);
		if (pv_parse_spec(&custom_user_spec, &custom_user_avp) == 0
				&& (custom_user_avp.type != PVT_AVP)) {
			LM_ERR("malformed or non AVP custom_user "
					"AVP definition in '%.*s'\n", custom_user_spec.len,custom_user_spec.s);
			return -1;
		}
	}

	init_custom_user(custom_user_spec.s ? &custom_user_avp : 0);

	return 0;
}


static void mod_destroy(void)
{
	destroy_rrcb_lists();
}


static int it_list_fixup(void** param, int param_no)
{
	pv_elem_t *model;
	str s;
	if(*param)
	{
		s.s = (char*)(*param); s.len = strlen(s.s);
		if(pv_parse_format(&s, &model)<0)
		{
			LM_ERR("wrong format[%s]\n",(char*)(*param));
			return E_UNSPEC;
		}
		*param = (void*)model;
	}
	return 0;
}


static int direction_fixup(void** param, int param_no)
{
	char *s;
	int n;

	if (!append_fromtag) {
		LM_ERR("usage of \"is_direction\" function requires parameter"
				"\"append_fromtag\" enabled!!");
		return E_CFG;
	}
	if (param_no==1) {
		n = 0;
		s = (char*) *param;
		if ( strcasecmp(s,"downstream")==0 ) {
			n = RR_FLOW_DOWNSTREAM;
		} else if ( strcasecmp(s,"upstream")==0 ) {
			n = RR_FLOW_UPSTREAM;
		} else {
			LM_ERR("unknown direction '%s'\n",s);
			return E_CFG;
		}
		/* free string */
		pkg_free(*param);
		/* replace it with the flag */
		*param = (void*)(unsigned long)n;
	}
	return 0;
}

/**
 * wrapper for loose_route(msg)
 */
static int w_loose_route(struct sip_msg *msg, char *p1, char *p2)
{
	return loose_route(msg);
}

/**
 * wrapper for record_route(msg, params)
 */
static int w_record_route(struct sip_msg *msg, char *key, char *bar)
{
	str s;

	if (msg->id == last_rr_msg) {
		LM_ERR("Double attempt to record-route\n");
		return -1;
	}

	if (key && pv_printf_s(msg, (pv_elem_t*)key, &s)<0) {
		LM_ERR("failed to print the format\n");
		return -1;
	}
	if ( record_route( msg, key?&s:0 )<0 )
		return -1;

	last_rr_msg = msg->id;
	return 1;
}


static int w_record_route_preset(struct sip_msg *msg, char *key, char *key2)
{
	str s;

	if (msg->id == last_rr_msg) {
		LM_ERR("Duble attempt to record-route\n");
		return -1;
	}
	if (key2 && !enable_double_rr) {
		LM_ERR("Attempt to double record-route while 'enable_double_rr' param is disabled\n");
		return -1;
	}

	if (pv_printf_s(msg, (pv_elem_t*)key, &s)<0) {
		LM_ERR("failed to print the format\n");
		return -1;
	}
	if ( record_route_preset( msg, &s)<0 )
		return -1;

	if (!key2)
		goto done;

	if (pv_printf_s(msg, (pv_elem_t*)key2, &s)<0) {
		LM_ERR("failed to print the format\n");
		return -1;
	}
	if ( record_route_preset( msg, &s)<0 )
		return -1;

done:
	last_rr_msg = msg->id;
	return 1;
}


/**
 * wrapper for record_route(msg, params)
 */
static int w_record_route_advertised_address(struct sip_msg *msg, char *addr, char *bar)
{
	str s;

	if (msg->id == last_rr_msg) {
		LM_ERR("Double attempt to record-route\n");
		return -1;
	}

	if (pv_printf_s(msg, (pv_elem_t*)addr, &s) < 0) {
		LM_ERR("failed to print the format\n");
		return -1;
	}
	if ( record_route_advertised_address( msg, &s ) < 0)
		return -1;

	last_rr_msg = msg->id;
	return 1;
}


static int w_add_rr_param(struct sip_msg *msg, char *key, char *foo)
{
	str s;

	if (pv_printf_s(msg, (pv_elem_t*)key, &s)<0) {
		LM_ERR("failed to print the format\n");
		return -1;
	}
	return ((add_rr_param( msg, &s)==0)?1:-1);
}



static int w_check_route_param(struct sip_msg *msg,char *re, char *foo)
{
	return ((check_route_param(msg,(regex_t*)re)==0)?1:-1);
}



static int w_is_direction(struct sip_msg *msg,char *dir, char *foo)
{
	return ((is_direction(msg,(int)(long)dir)==0)?1:-1);
}


/*
 * Return the URI of the topmost Route-Header.
 */
static int
pv_get_route_uri_f(struct sip_msg *msg, pv_param_t *param,
		  pv_value_t *res)
{
	struct hdr_field* hdr;
	rr_t* rt;
	str uri;

	if (!msg) {
		LM_ERR("No message?!?\n");
		return -1;
	}

	/* Parse the message until the First-Route-Header: */
	if (parse_headers(msg, HDR_ROUTE_F, 0) == -1) {
		LM_ERR("while parsing message\n");
		return -1;
    	}
	
	if (!msg->route) {
		LM_INFO("No route header present.\n");
		return -1;
	}
	hdr = msg->route;

	/* Parse the contents of the header: */
	if (parse_rr(hdr) == -1) {
		LM_ERR("Error while parsing Route header\n");
                return -1;
	}


	/* Retrieve the Route-Header */	
	rt = (rr_t*)hdr->parsed;
	uri = rt->nameaddr.uri;

	return pv_get_strval(msg, param, res, &uri);
}

