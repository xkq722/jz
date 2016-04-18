/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2015 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/php_rand.h"
#include "ext/standard/info.h"
#include "ext/standard/crc32.h"
#include "php_jz.h"

#include "jz_common.h"
#include "jz_data.h"
#include "jz_buffer.h"
#include "aes.c"
#include "zlib.c"

/* If you declare any globals in php_jz.h uncomment this:
ZEND_DECLARE_MODULE_GLOBALS(jz)
*/

/* True global resources - no need for thread safety here */
static int le_jz;

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("jz.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_jz_globals, jz_globals)
    STD_PHP_INI_ENTRY("jz.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_jz_globals, jz_globals)
PHP_INI_END()
*/
/* }}} */

/* {{{ php_jz_init_globals
 */
/* Uncomment this function if you have INI entries
static void php_jz_init_globals(zend_jz_globals *jz_globals)
{
	jz_globals->global_value = 0;
	jz_globals->global_string = NULL;
}
*/

ZEND_BEGIN_ARG_INFO_EX(arg_info_jz_encrypt, 0, 0, 2)
	ZEND_ARG_INFO(0, encrypt_str)
	ZEND_ARG_INFO(0, encrypt_key)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arg_info_jz_decrypt, 0, 0, 2)
	ZEND_ARG_INFO(0, decrypt_str)
	ZEND_ARG_INFO(0, decrypt_key)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arg_info_jz_trace, 0, 0, 1)
	ZEND_ARG_INFO(0, func_name)
ZEND_END_ARG_INFO()


/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(jz)
{
	/* If you have INI entries, uncomment these lines
	REGISTER_INI_ENTRIES();
	*/

	JZ_STARTUP(data);

	JZ_RINIT_FUNCTION(buffer);
	JZ_STARTUP(buffer);

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(jz)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(jz)
{
#if defined(COMPILE_DL_JZ) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(jz)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(jz)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "jz support", "enabled");
	php_info_print_table_row(2, "version", PHP_JZ_VERSION);
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

PHP_FUNCTION(jz_version)
{
	RETURN_STRING(PHP_JZ_VERSION);
}

PHP_FUNCTION(jz_encrypt)
{
	char *encrypt_data = NULL, *encrypt_key=NULL;
	size_t encrypt_data_len, encrypt_key_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &encrypt_data, &encrypt_data_len, &encrypt_key, &encrypt_key_len) == FAILURE) {
		RETURN_NULL();
	}

	if (0 == encrypt_data_len
		|| 0 == encrypt_key_len) {
		RETURN_NULL();
	}

	int crc = 0^0xFFFFFFFF;
	int n = encrypt_data_len;

	while (n--) {
		crc = ((crc >> 8) & 0x00FFFFFF) ^ crc32tab[(crc ^ (*encrypt_data++)) & 0xFF ];
	}
	crc = crc^0xFFFFFFFF;

	//reset pointer to begin
	encrypt_data -= encrypt_data_len;

	//check to gzcompress
	int gzip = encrypt_data_len > 100 ? 1 : 0;

	char *aes_data;
	size_t aes_data_len = encrypt_data_len;

	if (gzip) {
		int level = -1;
		int encoding = 0x0f;
		zend_string *out = php_zlib_encode(encrypt_data, encrypt_data_len, encoding, level);
		if (!out) {
			RETURN_NULL();
		}
		aes_data_len = out->len;
		aes_data = estrndup(out->val, out->len);
		zend_string_free(out);
	} else {
		aes_data = estrndup(encrypt_data, encrypt_data_len);
	}

	//header
	char header[33];
	sprintf(header, "ok,%d,%lu,%u", gzip, aes_data_len, crc);
	size_t i;
	for (i = strlen(header); i < 32; i++) {
		header[i] = ' ';
	}
	header[32] = '\0';

	//rand iv
	char aes_iv[17];
	int j;
	for (j = 0; j < 16; j++) {
		int ch = rand() % 255;
		aes_iv[j] = (char)ch;
	}
	aes_iv[16] = '\0';

	int encrypt_times = (aes_data_len + 32) / 16 + 1;
	int paddings      = 16 - (aes_data_len + 32) % 16;
	int aes_buf_size  = (aes_data_len + 32) + paddings;

	char *result = (char *)emalloc(16 + sizeof(char) * aes_buf_size);
	memcpy(result, aes_iv, 16);

	char * aes_origin_buf = (char *)emalloc(sizeof(char) * aes_buf_size);
	memset(aes_origin_buf, 0, sizeof(char) * aes_buf_size);
	memcpy(aes_origin_buf, header, 32);
	memcpy(aes_origin_buf + 32, aes_data, aes_data_len);

	char * aes_finnal_buf = (char *)emalloc(sizeof(char) * aes_buf_size);
	memset(aes_finnal_buf, 0, sizeof(char) * aes_buf_size);

	char aes_key[17];
	memset(aes_key, ' ', sizeof(aes_key));
	if (encrypt_key_len < 16) {
		memcpy(aes_key, encrypt_key, encrypt_key_len);
	} else {
		memcpy(aes_key, encrypt_key, 16);
	}

	aes_context aes_ctx;
	aes_set_key((unsigned char *)aes_key, 16, &aes_ctx);

	if (SUCCESS != aes_cbc_encrypt((unsigned char *)aes_origin_buf, (unsigned char *)aes_finnal_buf, encrypt_times, (unsigned char *)aes_iv, &aes_ctx)) {
		efree(aes_data);
		efree(aes_origin_buf);
		efree(aes_finnal_buf);
		RETURN_NULL();
	}

	memcpy(result + 16, aes_finnal_buf, aes_buf_size);

	efree(aes_data);
	efree(aes_origin_buf);
	efree(aes_finnal_buf);

	RETVAL_STRINGL(result, 16 + aes_buf_size);
	efree(result);
}


PHP_FUNCTION(jz_decrypt)
{
	char *decrypt_data = NULL, *decrypt_key=NULL;
	size_t decrypt_data_len, decrypt_key_len;

	array_init(return_value);
	add_index_bool(return_value, 0, 1);
	add_index_string(return_value, 1, "");

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &decrypt_data, &decrypt_data_len, &decrypt_key, &decrypt_key_len) == FAILURE) {
		return;
	}

	if (decrypt_data_len < 48
		|| decrypt_data_len % 16 != 0
		|| 0 == decrypt_key_len) {
		return;
	}

	//iv
	char aes_iv[17];
	memcpy(aes_iv, decrypt_data, 16);
	aes_iv[16] = '\0';

	int aes_buf_size  = decrypt_data_len - 16;
	int decrypt_times = aes_buf_size / 16;

	char *aes_finnal_buf = (char *)emalloc(sizeof(char) * aes_buf_size);
	memset(aes_finnal_buf, 0, sizeof(char) * aes_buf_size);
	memcpy(aes_finnal_buf, decrypt_data + 16, sizeof(char) * aes_buf_size);

	char *aes_origin_buf = (char *)emalloc(sizeof(char) * aes_buf_size);
	memset(aes_origin_buf, 0, sizeof(char) * aes_buf_size);

	char aes_key[17];
	memset(aes_key, ' ', sizeof(aes_key));
	if (decrypt_key_len < 16) {
		memcpy(aes_key, decrypt_key, decrypt_key_len);
	} else {
		memcpy(aes_key, decrypt_key, 16);
	}

	aes_context aes_ctx;
	aes_set_key((unsigned char *)aes_key, 16, &aes_ctx);
	if (SUCCESS != aes_cbc_decrypt((unsigned char *)aes_finnal_buf, (unsigned char *)aes_origin_buf, decrypt_times, (unsigned char *)aes_iv, &aes_ctx)) {
		efree(aes_finnal_buf);
		efree(aes_origin_buf);
		return;
	}

	efree(aes_finnal_buf);

	char header[33];
	memcpy(header, aes_origin_buf, 32);
	header[32] = '\0';

	char* header_flags[4];
	char* flags = strtok(header, ",");
	int i;
	for (i = 0; i < 4; i++) {
		header_flags[i] = flags;
		flags = strtok(NULL, ",");
	}

	if (strcmp(header_flags[0], "ok") != 0) {
		efree(aes_origin_buf);
		return;
	}

	int is_zip = 0;
	if (strcmp(header_flags[1], "1") == 0) {
		is_zip = 1;
	}

	size_t origin_data_size = strtoul(header_flags[2], NULL, 10);

	char *origin_data;
	size_t origin_data_len = origin_data_size;

	if (is_zip) {
		int encoding = 0x0f;
		if (SUCCESS != php_zlib_decode(aes_origin_buf + 32, origin_data_size, &origin_data, &origin_data_len, encoding, 0)) {
			efree(aes_origin_buf);
			return;
		}
	} else {
		origin_data = estrndup(aes_origin_buf + 32, origin_data_len);
	}

	efree(aes_origin_buf);

	int crc = 0^0xFFFFFFFF;
	size_t n = origin_data_len;

	while (n--) {
		crc = ((crc >> 8) & 0x00FFFFFF) ^ crc32tab[(crc ^ (*origin_data++)) & 0xFF ];
	}
	crc = crc^0xFFFFFFFF;

	origin_data -= origin_data_len;

	//fixed 32bit crc error
	char crc_str[1 + strlen(header_flags[3])];
	int crc_str_len = sprintf(crc_str, "%u", crc);
	crc_str[crc_str_len] = '\0';

	if (atoi(crc_str) != atoi(header_flags[3])) {
		efree(origin_data);
		return;
	}

	add_index_bool(return_value, 0, 0);
	add_index_stringl(return_value, 1, origin_data, origin_data_len);
	efree(origin_data);
}

PHP_FUNCTION(jz_trace)
{
	zval *callback, *args;
	int i = 0, argc = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z*", &callback, &args, &argc) == FAILURE) {
		RETURN_FALSE;
	}

	if (!zend_is_callable(callback, 0, NULL)) {
		RETURN_FALSE;
	}

	int paramc = 2 + argc;
	zval *params = safe_emalloc(sizeof(zval), paramc, 0);;
	zval retval;

	ZVAL_STRING(&params[0], zend_get_executed_filename());
	ZVAL_LONG(&params[1], zend_get_executed_lineno());

	for (i = 0; i < argc; i++) {
		ZVAL_COPY(&params[2 + i], &args[i]);
	}

	int status = call_user_function_ex(EG(function_table), NULL, callback, &retval, paramc, params, 0, NULL);

	if (status == SUCCESS && !Z_ISUNDEF(retval)) {
		RETVAL_ZVAL(&retval, 0, 0);
	} else {
		RETVAL_FALSE;
	}

	for (i = 0; i < paramc; i++) {
		zval_ptr_dtor(&params[i]);
	}

	efree(params);
}

/* {{{ jz_functions[]
 *
 * Every user visible function must have an entry in jz_functions[].
 */
const zend_function_entry jz_functions[] = {
	PHP_FE(jz_version, NULL)
	PHP_FE(jz_encrypt, arg_info_jz_encrypt)
	PHP_FE(jz_decrypt, arg_info_jz_decrypt)
	PHP_FE(jz_trace,   arg_info_jz_trace)
	PHP_FE_END	/* Must be the last line in jz_functions[] */
};
/* }}} */

/* {{{ jz_module_entry
 */
zend_module_entry jz_module_entry = {
	STANDARD_MODULE_HEADER,
	"jz",
	jz_functions,
	PHP_MINIT(jz),
	PHP_MSHUTDOWN(jz),
	PHP_RINIT(jz),
	PHP_RSHUTDOWN(jz),
	PHP_MINFO(jz),
	PHP_JZ_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_JZ
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE();
#endif
ZEND_GET_MODULE(jz)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
