/*
   +----------------------------------------------------------------------+
   | PHP version 4.0                                                      |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2001 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.02 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available at through the world-wide-web at                           |
   | http://www.php.net/license/2_02.txt.                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Rasmus Lerdorf <rasmus@php.net>                             |
   | (with helpful hints from Dean Gaudet <dgaudet@arctic.org>            |
   | PHP 4.0 patches by Zeev Suraski <zeev@zend.com>                      |
   +----------------------------------------------------------------------+
 */
/* $Id$ */

#define NO_REGEX_EXTRA_H
#ifdef WIN32
#include <winsock2.h>
#include <stddef.h>
#endif

#include "zend.h"
#include "php.h"
#include "php_variables.h"

#include "httpd.h"
#include "http_config.h"
#if MODULE_MAGIC_NUMBER > 19980712
# include "ap_compat.h"
#else
# if MODULE_MAGIC_NUMBER > 19980324
#  include "compat.h"
# endif
#endif
#include "http_core.h"
#include "http_main.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_log.h"

#include "php_ini.h"
#include "php_globals.h"
#include "SAPI.h"
#include "php_main.h"

#include "zend_compile.h"
#include "zend_execute.h"
#include "zend_highlight.h"
#include "zend_indent.h"

#include "ext/standard/php_standard.h"

#include "util_script.h"

#include "mod_php4.h"

#undef shutdown

/* {{{ Prototypes
 */
int apache_php_module_main(request_rec *r, int display_source_mode TSRMLS_DC);
void php_save_umask(void);
void php_restore_umask(void);
int sapi_apache_read_post(char *buffer, uint count_bytes TSRMLS_DC);
char *sapi_apache_read_cookies(TSRMLS_D);
int sapi_apache_header_handler(sapi_header_struct *sapi_header, sapi_headers_struct *sapi_headers TSRMLS_DC);
int sapi_apache_send_headers(sapi_headers_struct *sapi_headers TSRMLS_DC);
static int send_php(request_rec *r, int display_source_mode, char *filename);
static int send_parsed_php(request_rec * r);
static int send_parsed_php_source(request_rec * r);
int php_xbithack_handler(request_rec * r);
void php_init_handler(server_rec *s, pool *p);
/* }}} */

#if MODULE_MAGIC_NUMBER >= 19970728
static void php_child_exit_handler(server_rec *s, pool *p);
#endif

#if MODULE_MAGIC_NUMBER > 19961007
#define CONST_PREFIX const
#else
#define CONST_PREFIX
#endif
CONST_PREFIX char *php_apache_value_handler_ex(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2, int mode);
CONST_PREFIX char *php_apache_value_handler(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2);
CONST_PREFIX char *php_apache_admin_value_handler(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2);
CONST_PREFIX char *php_apache_flag_handler(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2);
CONST_PREFIX char *php_apache_flag_handler_ex(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2, int mode);
CONST_PREFIX char *php_apache_admin_flag_handler(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2);

/* ### these should be defined in mod_php4.h or somewhere else */
#define USE_PATH 1
#define IGNORE_URL 2

module MODULE_VAR_EXPORT php4_module;

int saved_umask;
static unsigned char apache_php_initialized;

typedef struct _php_per_dir_entry {
	char *key;
	char *value;
	uint key_length;
	uint value_length;
	int type;
} php_per_dir_entry;

/* some systems are missing these from their header files */

/* {{{ php_save_umask
 */
void php_save_umask(void)
{
	saved_umask = umask(077);
	umask(saved_umask);
}
/* }}} */

/* {{{ sapi_apache_ub_write
 */
static int sapi_apache_ub_write(const char *str, uint str_length TSRMLS_DC)
{
	int ret=0;
		
	if (SG(server_context)) {
		ret = rwrite(str, str_length, (request_rec *) SG(server_context));
	}
	if (ret != str_length) {
		php_handle_aborted_connection();
	}
	return ret;
}
/* }}} */

/* {{{ sapi_apache_flush
 */
static void sapi_apache_flush(void *server_context)
{
	if (server_context) {
#if MODULE_MAGIC_NUMBER > 19970110
		rflush((request_rec *) server_context);
#else
		bflush((request_rec *) server_context->connection->client);
#endif
	}
}
/* }}} */

/* {{{ sapi_apache_read_post
 */
int sapi_apache_read_post(char *buffer, uint count_bytes TSRMLS_DC)
{
	uint total_read_bytes=0, read_bytes;
	request_rec *r = (request_rec *) SG(server_context);
	void (*handler)(int);

	handler = signal(SIGPIPE, SIG_IGN);
	while (total_read_bytes<count_bytes) {
		hard_timeout("Read POST information", r); /* start timeout timer */
		read_bytes = get_client_block(r, buffer+total_read_bytes, count_bytes-total_read_bytes);
		reset_timeout(r);
		if (read_bytes<=0) {
			break;
		}
		total_read_bytes += read_bytes;
	}
	signal(SIGPIPE, handler);	
	return total_read_bytes;
}
/* }}} */

/* {{{ sapi_apache_read_cookies
 */
char *sapi_apache_read_cookies(TSRMLS_D)
{
	return (char *) table_get(((request_rec *) SG(server_context))->subprocess_env, "HTTP_COOKIE");
}
/* }}} */

/* {{{ sapi_apache_header_handler
 */
int sapi_apache_header_handler(sapi_header_struct *sapi_header, sapi_headers_struct *sapi_headers TSRMLS_DC)
{
	char *header_name, *header_content, *p;
	request_rec *r = (request_rec *) SG(server_context);

	header_name = sapi_header->header;

	header_content = p = strchr(header_name, ':');
	if (!p) {
		return 0;
	}

	*p = 0;
	do {
		header_content++;
	} while (*header_content==' ');

	if (!strcasecmp(header_name, "Content-Type")) {
		r->content_type = pstrdup(r->pool, header_content);
	} else if (!strcasecmp(header_name, "Set-Cookie")) {
		table_add(r->headers_out, header_name, header_content);
	} else {
		table_set(r->headers_out, header_name, header_content);
	}

	*p = ':';  /* a well behaved header handler shouldn't change its original arguments */

	efree(sapi_header->header);
	
	return 0;  /* don't use the default SAPI mechanism, Apache duplicates this functionality */
}
/* }}} */

/* {{{ sapi_apache_send_headers
 */
int sapi_apache_send_headers(sapi_headers_struct *sapi_headers TSRMLS_DC)
{
	if(SG(server_context) == NULL) { /* server_context is not here anymore */
		return SAPI_HEADER_SEND_FAILED;
	}

	((request_rec *) SG(server_context))->status = SG(sapi_headers).http_response_code;
	send_http_header((request_rec *) SG(server_context));
	return SAPI_HEADER_SENT_SUCCESSFULLY;
}
/* }}} */

/* {{{ sapi_apache_register_server_variables
 */
static void sapi_apache_register_server_variables(zval *track_vars_array TSRMLS_DC)
{
	register int i;
	array_header *arr = table_elts(((request_rec *) SG(server_context))->subprocess_env);
	table_entry *elts = (table_entry *) arr->elts;
	zval **path_translated;
	HashTable *symbol_table;

	for (i = 0; i < arr->nelts; i++) {
		char *val;

		if (elts[i].val) {
			val = elts[i].val;
		} else {
			val = empty_string;
		}
		php_register_variable(elts[i].key, val, track_vars_array  TSRMLS_CC);
	}

	/* If PATH_TRANSLATED doesn't exist, copy it from SCRIPT_FILENAME */
	if (track_vars_array) {
		symbol_table = track_vars_array->value.ht;
	} else if (PG(register_globals)) {
		/* should never happen nowadays */
		symbol_table = EG(active_symbol_table);
	} else {
		symbol_table = NULL;
	}
	if (symbol_table
		&& !zend_hash_exists(symbol_table, "PATH_TRANSLATED", sizeof("PATH_TRANSLATED"))
		&& zend_hash_find(symbol_table, "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME"), (void **) &path_translated)==SUCCESS) {
		php_register_variable("PATH_TRANSLATED", Z_STRVAL_PP(path_translated), track_vars_array TSRMLS_CC);
	}

	php_register_variable("PHP_SELF", ((request_rec *) SG(server_context))->uri, track_vars_array TSRMLS_CC);
}
/* }}} */

/* {{{ php_apache_startup
 */
static int php_apache_startup(sapi_module_struct *sapi_module)
{
	if (php_module_startup(sapi_module) == FAILURE
		|| zend_startup_module(&apache_module_entry) == FAILURE) {
		return FAILURE;
	} else {
		return SUCCESS;
	}
}
/* }}} */

/* {{{ php_apache_log_message
 */
static void php_apache_log_message(char *message)
{
	TSRMLS_FETCH();

	if (SG(server_context)) {
#if MODULE_MAGIC_NUMBER >= 19970831
		aplog_error(NULL, 0, APLOG_ERR | APLOG_NOERRNO, ((request_rec *) SG(server_context))->server, "%s", message);
#else
		log_error(message, ((request_rec *) SG(server_context))->server);
#endif
	} else {
		fprintf(stderr, "%s", message);
		fprintf(stderr, "\n");
	}
}
/* }}} */

/* {{{ php_apache_request_shutdown
 */
static void php_apache_request_shutdown(void *dummy)
{
	TSRMLS_FETCH();

	php_output_set_status(0 TSRMLS_CC);
	SG(server_context) = NULL; /* The server context (request) is invalid by the time run_cleanups() is called */
	if (AP(in_request)) {
		AP(in_request) = 0;
		php_request_shutdown(dummy);
	}
}
/* }}} */

/* {{{ php_apache_sapi_activate
 */
static int php_apache_sapi_activate(TSRMLS_D)
{
	request_rec *r = (request_rec *) SG(server_context); 

	/*
	 * For the Apache module version, this bit of code registers a cleanup
	 * function that gets triggered when our request pool is destroyed.
	 * We need this because at any point in our code we can be interrupted
	 * and that may happen before we have had time to free our memory.
	 * The php_request_shutdown function needs to free all outstanding allocated
	 * memory.  
	 */
	block_alarms();
	register_cleanup(r->pool, NULL, php_apache_request_shutdown, php_request_shutdown_for_exec);
	AP(in_request)=1;
	unblock_alarms();

	/* Override the default headers_only value - sometimes "GET" requests should actually only
	 * send headers.
	 */
	SG(request_info).headers_only = r->header_only;
	return SUCCESS;
}
/* }}} */

/* {{{ php_apache_get_stat
 */
static struct stat *php_apache_get_stat(TSRMLS_D)
{
	return &((request_rec *) SG(server_context))->finfo;
}
/* }}} */

/* {{{ php_apache_getenv
 */
static char *php_apache_getenv(char *name, size_t name_len TSRMLS_DC)
{
	return (char *) table_get(((request_rec *) SG(server_context))->subprocess_env, name);
}
/* }}} */

/* {{{ sapi_module_struct apache_sapi_module
 */
static sapi_module_struct apache_sapi_module = {
	"apache",						/* name */
	"Apache",						/* pretty name */
									
	php_apache_startup,				/* startup */
	php_module_shutdown_wrapper,	/* shutdown */

	php_apache_sapi_activate,		/* activate */
	NULL,							/* deactivate */

	sapi_apache_ub_write,			/* unbuffered write */
	sapi_apache_flush,				/* flush */
	php_apache_get_stat,			/* get uid */
	php_apache_getenv,				/* getenv */

	php_error,						/* error handler */

	sapi_apache_header_handler,		/* header handler */
	sapi_apache_send_headers,		/* send headers handler */
	NULL,							/* send header handler */

	sapi_apache_read_post,			/* read POST data */
	sapi_apache_read_cookies,		/* read Cookies */

	sapi_apache_register_server_variables,		/* register server variables */
	php_apache_log_message,			/* Log message */

	NULL,					/* php.ini path override */

#ifdef PHP_WIN32
	NULL,
	NULL,
#else
	block_alarms,					/* Block interruptions */
	unblock_alarms,					/* Unblock interruptions */
#endif

	STANDARD_SAPI_MODULE_PROPERTIES
};
/* }}} */

/* {{{ php_restore_umask
 */
void php_restore_umask(void)
{
	umask(saved_umask);
}
/* }}} */

/* {{{ init_request_info
 */
static void init_request_info(request_rec *r TSRMLS_DC)
{
	char *content_length = (char *) table_get(r->subprocess_env, "CONTENT_LENGTH");
	const char *authorization=NULL;
	char *tmp;

	SG(request_info).query_string = r->args;
	SG(request_info).path_translated = r->filename;
	SG(request_info).request_uri = r->uri;
	SG(request_info).request_method = (char *)r->method;
	SG(request_info).content_type = (char *) table_get(r->subprocess_env, "CONTENT_TYPE");
	SG(request_info).content_length = (content_length ? atoi(content_length) : 0);
	SG(sapi_headers).http_response_code = r->status;

	if (r->headers_in) {
		authorization = table_get(r->headers_in, "Authorization");
	}
	if (authorization
/* 		&& !auth_type(r) */
		&& !strcasecmp(getword(r->pool, &authorization, ' '), "Basic")) {
		tmp = uudecode(r->pool, authorization);
		SG(request_info).auth_user = getword_nulls_nc(r->pool, &tmp, ':');
		if (SG(request_info).auth_user) {
			r->connection->user = pstrdup(r->connection->pool, SG(request_info).auth_user);
			r->connection->ap_auth_type = "Basic";
			SG(request_info).auth_user = estrdup(SG(request_info).auth_user);
		}
		SG(request_info).auth_password = tmp;
		if (SG(request_info).auth_password) {
			SG(request_info).auth_password = estrdup(SG(request_info).auth_password);
		}
	} else {
		SG(request_info).auth_user = NULL;
		SG(request_info).auth_password = NULL;
	}
}
/* }}} */

/* {{{ php_apache_alter_ini_entries
 */
static int php_apache_alter_ini_entries(php_per_dir_entry *per_dir_entry TSRMLS_DC)
{
	zend_alter_ini_entry(per_dir_entry->key, per_dir_entry->key_length+1, per_dir_entry->value, per_dir_entry->value_length, per_dir_entry->type, PHP_INI_STAGE_ACTIVATE);
	return 0;
}
/* }}} */

/* {{{ php_apache_get_default_mimetype
 */
static char *php_apache_get_default_mimetype(request_rec *r TSRMLS_DC)
{
	
	char *mimetype;
	if (SG(default_mimetype) || SG(default_charset)) {
		/* Assume output will be of the default MIME type.  Individual
		   scripts may change this later. */
		char *tmpmimetype;
		tmpmimetype = sapi_get_default_content_type(TSRMLS_C);
		mimetype = pstrdup(r->pool, tmpmimetype);
		efree(tmpmimetype);
	} else {
		mimetype = SAPI_DEFAULT_MIMETYPE "; charset=" SAPI_DEFAULT_CHARSET;
	}
	return mimetype;
}
/* }}} */

/* {{{ send_php
 */
static int send_php(request_rec *r, int display_source_mode, char *filename)
{
	int retval;
	HashTable *per_dir_conf;
	TSRMLS_FETCH();

	if (AP(in_request)) {
		zend_file_handle fh;

		fh.filename = r->filename;
		fh.opened_path = NULL;
		fh.free_filename = 0;
		fh.type = ZEND_HANDLE_FILENAME;
		zend_execute_scripts(ZEND_INCLUDE TSRMLS_CC, NULL, 1, &fh);
		return OK;
	}

	zend_first_try {
		/* We don't accept OPTIONS requests, but take everything else */
		if (r->method_number == M_OPTIONS) {
			r->allowed |= (1 << METHODS) - 1;
			return DECLINED;
		}

		/* Make sure file exists */
		if (filename == NULL && r->finfo.st_mode == 0) {
			return DECLINED;
		}

		if(!AP(apache_config_loaded)) {
			per_dir_conf = (HashTable *) get_module_config(r->per_dir_config, &php4_module);
			if (per_dir_conf) {
				zend_hash_apply((HashTable *) per_dir_conf, (apply_func_t) php_apache_alter_ini_entries TSRMLS_CC);
			}
			AP(apache_config_loaded) = 1;
		}

		/* If PHP parser engine has been turned off with an "engine off"
		 * directive, then decline to handle this request
		 */
		if (!AP(engine)) {
			r->content_type = php_apache_get_default_mimetype(r TSRMLS_CC);
			r->allowed |= (1 << METHODS) - 1;
			zend_try {
				zend_ini_deactivate(TSRMLS_C);
			} zend_end_try();
			return DECLINED;
		}
		if (filename == NULL) {
			filename = r->filename;
		}

		/* Apache 1.2 has a more complex mechanism for reading POST data */
#if MODULE_MAGIC_NUMBER > 19961007
		if ((retval = setup_client_block(r, REQUEST_CHUNKED_ERROR))) {
			zend_try {
				zend_ini_deactivate(TSRMLS_C);
			} zend_end_try();
			return retval;
		}
#endif

		if (AP(last_modified)) {
#if MODULE_MAGIC_NUMBER < 19970912
			if ((retval = set_last_modified(r, r->finfo.st_mtime))) {
				zend_try {
					zend_ini_deactivate(TSRMLS_C);
				} zend_end_try();
				return retval;
			}
#else
			update_mtime (r, r->finfo.st_mtime);
			set_last_modified(r);
			set_etag(r);
#endif
		}
		/* Assume output will be of the default MIME type.  Individual
		   scripts may change this later in the request. */
		r->content_type = php_apache_get_default_mimetype(r TSRMLS_CC);

		/* Init timeout */
		hard_timeout("send", r);

		SG(server_context) = r;
		
		php_save_umask();
		add_common_vars(r);
		add_cgi_vars(r);

		init_request_info(r TSRMLS_CC);
		apache_php_module_main(r, display_source_mode TSRMLS_CC);

		/* Done, restore umask, turn off timeout, close file and return */
		php_restore_umask();
		kill_timeout(r);
	} zend_end_try();

	return OK;
}
/* }}} */

/* {{{ send_parsed_php
 */
static int send_parsed_php(request_rec * r)
{
	int result =  send_php(r, 0, NULL);

#if MEMORY_LIMIT
    {
        char mem_usage[ 32 ];
        TSRMLS_FETCH();
 
        sprintf(mem_usage,"%u", (int) AG(allocated_memory_peak));
        ap_table_setn(r->notes, "mod_php_memory_usage", ap_pstrdup(r->pool, mem_usage));
    }
#endif

	return result;
}
/* }}} */

/* {{{ send_parsed_php_source
 */
static int send_parsed_php_source(request_rec * r)
{
	return send_php(r, 1, NULL);
}
/* }}} */

/* {{{ destroy_per_dir_entry
 */
static void destroy_per_dir_entry(php_per_dir_entry *per_dir_entry)
{
	free(per_dir_entry->key);
	free(per_dir_entry->value);
}
/* }}} */

/* {{{ copy_per_dir_entry
 */
static void copy_per_dir_entry(php_per_dir_entry *per_dir_entry)
{
	php_per_dir_entry tmp = *per_dir_entry;

	per_dir_entry->key = (char *) malloc(tmp.key_length+1);
	memcpy(per_dir_entry->key, tmp.key, tmp.key_length);
	per_dir_entry->key[per_dir_entry->key_length] = 0;

	per_dir_entry->value = (char *) malloc(tmp.value_length+1);
	memcpy(per_dir_entry->value, tmp.value, tmp.value_length);
	per_dir_entry->value[per_dir_entry->value_length] = 0;
}
/* }}} */

/* {{{ should_overwrite_per_dir_entry
 */
static zend_bool should_overwrite_per_dir_entry(php_per_dir_entry *orig_per_dir_entry, php_per_dir_entry *new_per_dir_entry)
{
	if (new_per_dir_entry->type==PHP_INI_SYSTEM
		&& orig_per_dir_entry->type!=PHP_INI_SYSTEM) {
		return 1;
	} else {
		return 0;
	}
}
/* }}} */

/* {{{ php_destroy_per_dir_info
 */
static void php_destroy_per_dir_info(HashTable *per_dir_info)
{
	zend_hash_destroy(per_dir_info);
	free(per_dir_info);
}
/* }}} */

/* {{{ php_create_dir
 */
static void *php_create_dir(pool *p, char *dummy)
{
	HashTable *per_dir_info;

	per_dir_info = (HashTable *) malloc(sizeof(HashTable));
	zend_hash_init(per_dir_info, 5, NULL, (void (*)(void *)) destroy_per_dir_entry, 1);
	register_cleanup(p, (void *) per_dir_info, (void (*)(void *)) php_destroy_per_dir_info, (void (*)(void *)) zend_hash_destroy);

	return per_dir_info;
}
/* }}} */

/* {{{ php_merge_dir
 */
static void *php_merge_dir(pool *p, void *basev, void *addv)
{
	/* This function *must* return addv, and not modify basev */
	zend_hash_merge_ex((HashTable *) addv, (HashTable *) basev, (copy_ctor_func_t) copy_per_dir_entry, sizeof(php_per_dir_entry), (zend_bool (*)(void *, void *)) should_overwrite_per_dir_entry);
	return addv;
}
/* }}} */

/* {{{ php_apache_value_handler_ex
 */
CONST_PREFIX char *php_apache_value_handler_ex(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2, int mode)
{
	php_per_dir_entry per_dir_entry;

	if (!apache_php_initialized) {
		apache_php_initialized = 1;
#ifdef ZTS
		tsrm_startup(1, 1, 0, NULL);
#endif
		sapi_startup(&apache_sapi_module);
		php_apache_startup(&apache_sapi_module);
	}
	per_dir_entry.type = mode;

	if (strcasecmp(arg2, "none") == 0) {
		arg2 = "";
	}

	per_dir_entry.key_length = strlen(arg1);
	per_dir_entry.value_length = strlen(arg2);

	per_dir_entry.key = (char *) malloc(per_dir_entry.key_length+1);
	memcpy(per_dir_entry.key, arg1, per_dir_entry.key_length);
	per_dir_entry.key[per_dir_entry.key_length] = 0;

	per_dir_entry.value = (char *) malloc(per_dir_entry.value_length+1);
	memcpy(per_dir_entry.value, arg2, per_dir_entry.value_length);
	per_dir_entry.value[per_dir_entry.value_length] = 0;

	zend_hash_update((HashTable *) conf, per_dir_entry.key, per_dir_entry.key_length, &per_dir_entry, sizeof(php_per_dir_entry), NULL);
	return NULL;
}
/* }}} */

/* {{{ php_apache_value_handler
 */
CONST_PREFIX char *php_apache_value_handler(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2)
{
	return php_apache_value_handler_ex(cmd, conf, arg1, arg2, PHP_INI_PERDIR);
}
/* }}} */

/* {{{ php_apache_admin_value_handler
 */
CONST_PREFIX char *php_apache_admin_value_handler(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2)
{
	return php_apache_value_handler_ex(cmd, conf, arg1, arg2, PHP_INI_SYSTEM);
}
/* }}} */

/* {{{ php_apache_flag_handler_ex
 */
CONST_PREFIX char *php_apache_flag_handler_ex(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2, int mode)
{
	char bool_val[2];

	if (!strcasecmp(arg2, "On")) {
		bool_val[0] = '1';
	} else {
		bool_val[0] = '0';
	}
	bool_val[1] = 0;
	
	return php_apache_value_handler_ex(cmd, conf, arg1, bool_val, mode);
}
/* }}} */

/* {{{ php_apache_flag_handler
 */
CONST_PREFIX char *php_apache_flag_handler(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2)
{
	return php_apache_flag_handler_ex(cmd, conf, arg1, arg2, PHP_INI_PERDIR);
}
/* }}} */

/* {{{ php_apache_admin_flag_handler
 */
CONST_PREFIX char *php_apache_admin_flag_handler(cmd_parms *cmd, HashTable *conf, char *arg1, char *arg2)
{
	return php_apache_flag_handler_ex(cmd, conf, arg1, arg2, PHP_INI_SYSTEM);
}
/* }}} */

/* {{{ int php_xbithack_handler(request_rec * r)
 */
int php_xbithack_handler(request_rec * r)
{
    HashTable *conf;

	if(!AP(apache_config_loaded)) {
		conf = (HashTable *) get_module_config(r->per_dir_config, &php4_module);
		if (conf) {
			zend_hash_apply((HashTable *)conf, (apply_func_t) php_apache_alter_ini_entries TSRMLS_CC);
		}
		AP(apache_config_loaded) = 1;
	}

	if (!(r->finfo.st_mode & S_IXUSR)) {
		r->allowed |= (1 << METHODS) - 1;
		return DECLINED;
	}
	if (!AP(xbithack)) {
		r->allowed |= (1 << METHODS) - 1;
		return DECLINED;
	}
	return send_parsed_php(r);
}
/* }}} */

/* {{{ apache_php_module_shutdown_wrapper
 */
static void apache_php_module_shutdown_wrapper(void)
{
	apache_php_initialized = 0;
	apache_sapi_module.shutdown(&apache_sapi_module);

#if MODULE_MAGIC_NUMBER >= 19970728
	/* This function is only called on server exit if the apache API
	 * child_exit handler exists, so shutdown globally 
	 */
	sapi_shutdown();
#endif

#ifdef ZTS
	tsrm_shutdown();
#endif
}
/* }}} */

#if MODULE_MAGIC_NUMBER >= 19970728
/* {{{ php_child_exit_handler
 */
static void php_child_exit_handler(server_rec *s, pool *p)
{
/*	apache_php_initialized = 0; */
	apache_sapi_module.shutdown(&apache_sapi_module);

#ifdef ZTS
	tsrm_shutdown();
#endif
}
/* }}} */
#endif

/* {{{ void php_init_handler(server_rec *s, pool *p)
 */
void php_init_handler(server_rec *s, pool *p)
{
	register_cleanup(p, NULL, (void (*)(void *))apache_php_module_shutdown_wrapper, (void (*)(void *))php_module_shutdown_for_exec);
	if (!apache_php_initialized) {
		apache_php_initialized = 1;
#ifdef ZTS
		tsrm_startup(1, 1, 0, NULL);
#endif
		sapi_startup(&apache_sapi_module);
		php_apache_startup(&apache_sapi_module);
	}
#if MODULE_MAGIC_NUMBER >= 19980527
	{
		TSRMLS_FETCH();
		if (PG(expose_php)) {
			ap_add_version_component("PHP/" PHP_VERSION);
		}
	}
#endif
}
/* }}} */

/* {{{ handler_rec php_handlers[]
 */
handler_rec php_handlers[] =
{
	{"application/x-httpd-php", send_parsed_php},
	{"application/x-httpd-php-source", send_parsed_php_source},
	{"text/html", php_xbithack_handler},
	{NULL}
};
/* }}} */

/* {{{ command_rec php_commands[]
 */
command_rec php_commands[] =
{
	{"php_value",		php_apache_value_handler, NULL, OR_OPTIONS, TAKE2, "PHP Value Modifier"},
	{"php_flag",		php_apache_flag_handler, NULL, OR_OPTIONS, TAKE2, "PHP Flag Modifier"},
	{"php_admin_value",	php_apache_admin_value_handler, NULL, ACCESS_CONF|RSRC_CONF, TAKE2, "PHP Value Modifier (Admin)"},
	{"php_admin_flag",	php_apache_admin_flag_handler, NULL, ACCESS_CONF|RSRC_CONF, TAKE2, "PHP Flag Modifier (Admin)"},
	{NULL}
};
/* }}} */

static int php_uri_translation(request_rec *r)
{
	char *handler = NULL;
	zval *ret = NULL;
    HashTable *conf;
	TSRMLS_FETCH();

	if(!AP(apache_config_loaded)) {
		conf = (HashTable *) get_module_config(r->per_dir_config, &php4_module);
		if (conf) {
			zend_hash_apply((HashTable *)conf, (apply_func_t) php_apache_alter_ini_entries TSRMLS_CC);
		}
		AP(apache_config_loaded) = 1;
	}

	handler = AP(uri_handler);

	if(handler) {
		hard_timeout("send", r);
		SG(server_context) = r;
		php_save_umask();
		add_common_vars(r);
		add_cgi_vars(r);
		init_request_info(r TSRMLS_CC);
		apache_php_module_hook(r, handler, &ret TSRMLS_CC);
		php_restore_umask();
		kill_timeout(r);
		convert_to_string(ret);
		if(Z_STRLEN_P(ret)) {
			if (strchr(Z_STRVAL_P(ret), ':')) {
				ap_table_setn(r->headers_out, "Location", Z_STRVAL_P(ret));
				return REDIRECT;
			} else {
				r->filename = ap_pstrdup(r->pool, Z_STRVAL_P(ret));
			}
			return OK;
		} else {
			return DECLINED;
		}
	} else {
		return DECLINED;
	}
}

/* {{{ module MODULE_VAR_EXPORT php4_module
 */
module MODULE_VAR_EXPORT php4_module =
{
	STANDARD_MODULE_STUFF,
	php_init_handler,			/* initializer */
	php_create_dir,				/* per-directory config creator */
	php_merge_dir,				/* dir merger */
	NULL,						/* per-server config creator */
	NULL, 						/* merge server config */
	php_commands,				/* command table */
	php_handlers,				/* handlers */
	php_uri_translation,		/* filename translation */
	NULL,						/* check_user_id */
	NULL,						/* check auth */
	NULL,						/* check access */
	NULL,						/* type_checker */
	NULL,						/* fixups */
	NULL						/* logger */
#if MODULE_MAGIC_NUMBER >= 19970103
	, NULL						/* header parser */
#endif
#if MODULE_MAGIC_NUMBER >= 19970719
	, NULL             			/* child_init */
#endif
#if MODULE_MAGIC_NUMBER >= 19970728
	, php_child_exit_handler		/* child_exit */
#endif
#if MODULE_MAGIC_NUMBER >= 19970902
	, NULL						/* post read-request */
#endif
};
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 tw=78 fdm=marker
 * vim<600: sw=4 ts=4 tw=78
 */
