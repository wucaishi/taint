/*
   +----------------------------------------------------------------------+
   | Taint                                                                |
   +----------------------------------------------------------------------+
   | Copyright (c) 2012-2015 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author:  Xinchen Hui    <laruence@php.net>                           |
   +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "SAPI.h"
#include "zend_compile.h"
#include "zend_execute.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_taint.h"

ZEND_DECLARE_MODULE_GLOBALS(taint)

/* {{{ TAINT_ARG_INFO
*/
ZEND_BEGIN_ARG_INFO_EX(taint_arginfo, 0, 0, 1)
	ZEND_ARG_INFO(1, string)
	ZEND_ARG_INFO(1, ...)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(untaint_arginfo, 0, 0, 1)
	ZEND_ARG_INFO(1, string)
	ZEND_ARG_INFO(1, ...)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(is_tainted_arginfo, 0, 0, 1)
	ZEND_ARG_INFO(0, string)
ZEND_END_ARG_INFO()
	/* }}} */

/* {{{ taint_functions[]
*/
zend_function_entry taint_functions[] = {
	PHP_FE(taint, taint_arginfo)
	PHP_FE(untaint, untaint_arginfo)
	PHP_FE(is_tainted, is_tainted_arginfo)
	{NULL, NULL, NULL}
};
/* }}} */

/** {{{ module depends
*/
zend_module_dep taint_deps[] = {
	ZEND_MOD_CONFLICTS("xdebug")
	{NULL, NULL, NULL}
};
/* }}} */

/* {{{ taint_module_entry
*/
zend_module_entry taint_module_entry = {
	STANDARD_MODULE_HEADER_EX, NULL,
	taint_deps,
	"taint",
	taint_functions,
	PHP_MINIT(taint),
	PHP_MSHUTDOWN(taint),
	PHP_RINIT(taint),
	PHP_RSHUTDOWN(taint),
	PHP_MINFO(taint),
	PHP_TAINT_VERSION,
	PHP_MODULE_GLOBALS(taint),
	NULL,
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

static struct taint_overridden_fucs /* {{{ */ {
	php_func strval;
	php_func sprintf;
	php_func vsprintf;
	php_func explode;
	php_func implode;
	php_func trim;
	php_func rtrim;
	php_func ltrim;
	php_func strstr;
	php_func str_pad;
	php_func str_replace;
	php_func substr;
	php_func strtolower;
	php_func strtoupper;
	php_func dirname;
	php_func basename;
	php_func pathinfo;
} taint_origin_funcs;

#define TAINT_O_FUNC(m) (taint_origin_funcs.m)
/* }}} */

/* These are most copied from zend_execute.c: zend_fetch_dimension_address */
static int php_taint_make_real_object(zval *object) /* {{{ */ {
	if (UNEXPECTED(Z_TYPE_P(object) != IS_OBJECT)) {
		if (EXPECTED(Z_TYPE_P(object) <= IS_FALSE)) {
			/* nothing to destroy */
		} else if (EXPECTED((Z_TYPE_P(object) == IS_STRING && Z_STRLEN_P(object) == 0))) {
			zval_ptr_dtor_nogc(object);
		} else {
			return 0;
		}
		object_init(object);
		zend_error(E_WARNING, "Creating default object from empty value");
	}
	return 1;
}
/* }}} */

static zend_long php_taint_check_string_offset(zval *dim, int type) /* {{{ */ {
	zend_long offset;

try_again:
	if (UNEXPECTED(Z_TYPE_P(dim) != IS_LONG)) {
		switch(Z_TYPE_P(dim)) {
			case IS_STRING:
				if (IS_LONG == is_numeric_string(Z_STRVAL_P(dim), Z_STRLEN_P(dim), NULL, NULL, -1)) {
					break;
				}
				if (type != BP_VAR_UNSET) {
					zend_error(E_WARNING, "Illegal string offset '%s'", Z_STRVAL_P(dim));
				}
				break;
			case IS_DOUBLE:
			case IS_NULL:
			case IS_FALSE:
			case IS_TRUE:
				zend_error(E_NOTICE, "String offset cast occurred");
				break;
			case IS_REFERENCE:
				dim = Z_REFVAL_P(dim);
				goto try_again;
			default:
				zend_error(E_WARNING, "Illegal offset type");
				break;
		}

		offset = zval_get_long(dim);
	} else {
		offset = Z_LVAL_P(dim);
	}

	return offset;
}
/* }}} */

static zval *php_taint_fetch_dimension_address_inner(HashTable *ht, const zval *dim, int dim_type, int type) /* {{{ */ {
	zval *retval;
	zend_string *offset_key;
	zend_ulong hval;

try_again:
	if (EXPECTED(Z_TYPE_P(dim) == IS_LONG)) {
		hval = Z_LVAL_P(dim);
num_index:
		retval = zend_hash_index_find(ht, hval);
		if (retval == NULL) {
			switch (type) {
				case BP_VAR_R:
					zend_error(E_NOTICE,"Undefined offset: " ZEND_LONG_FMT, hval);
					/* break missing intentionally */
				case BP_VAR_UNSET:
				case BP_VAR_IS:
					retval = &EG(uninitialized_zval);
					break;
				case BP_VAR_RW:
					zend_error(E_NOTICE,"Undefined offset: " ZEND_LONG_FMT, hval);
					/* break missing intentionally */
				case BP_VAR_W:
					retval = zend_hash_index_add_new(ht, hval, &EG(uninitialized_zval));
					break;
			}
		}
	} else if (EXPECTED(Z_TYPE_P(dim) == IS_STRING)) {
		offset_key = Z_STR_P(dim);
		if (dim_type != IS_CONST) {
			if (ZEND_HANDLE_NUMERIC(offset_key, hval)) {
				goto num_index;
			}
		}
str_index:
		retval = zend_hash_find(ht, offset_key);
		if (retval) {
			/* support for $GLOBALS[...] */
			if (UNEXPECTED(Z_TYPE_P(retval) == IS_INDIRECT)) {
				retval = Z_INDIRECT_P(retval);
				if (UNEXPECTED(Z_TYPE_P(retval) == IS_UNDEF)) {
					switch (type) {
						case BP_VAR_R:
							zend_error(E_NOTICE, "Undefined index: %s", ZSTR_VAL(offset_key));
							/* break missing intentionally */
						case BP_VAR_UNSET:
						case BP_VAR_IS:
							retval = &EG(uninitialized_zval);
							break;
						case BP_VAR_RW:
							zend_error(E_NOTICE,"Undefined index: %s", ZSTR_VAL(offset_key));
							/* break missing intentionally */
						case BP_VAR_W:
							ZVAL_NULL(retval);
							break;
					}
				}
			}
		} else {
			switch (type) {
				case BP_VAR_R:
					zend_error(E_NOTICE, "Undefined index: %s", ZSTR_VAL(offset_key));
					/* break missing intentionally */
				case BP_VAR_UNSET:
				case BP_VAR_IS:
					retval = &EG(uninitialized_zval);
					break;
				case BP_VAR_RW:
					zend_error(E_NOTICE,"Undefined index: %s", ZSTR_VAL(offset_key));
					/* break missing intentionally */
				case BP_VAR_W:
					retval = zend_hash_add_new(ht, offset_key, &EG(uninitialized_zval));
					break;
			}
		}
	} else {
		switch (Z_TYPE_P(dim)) {
			case IS_NULL:
				offset_key = ZSTR_EMPTY_ALLOC();
				goto str_index;
			case IS_DOUBLE:
				hval = zend_dval_to_lval(Z_DVAL_P(dim));
				goto num_index;
			case IS_RESOURCE:
				zend_error(E_NOTICE, "Resource ID#%pd used as offset, casting to integer (%pd)", Z_RES_HANDLE_P(dim), Z_RES_HANDLE_P(dim));
				hval = Z_RES_HANDLE_P(dim);
				goto num_index;
			case IS_FALSE:
				hval = 0;
				goto num_index;
			case IS_TRUE:
				hval = 1;
				goto num_index;
			case IS_REFERENCE:
				dim = Z_REFVAL_P(dim);
				goto try_again;
			default:
				zend_error(E_WARNING, "Illegal offset type");
				retval = (type == BP_VAR_W || type == BP_VAR_RW) ?
					&EG(error_zval) : &EG(uninitialized_zval);
		}
	}
	return retval;
}
/* }}} */

static void php_taint_fetch_dimension_address(zval *result, zval *container, zval *dim, int dim_type, int type) /* {{{ */ {
	zval *retval;

	if (EXPECTED(Z_TYPE_P(container) == IS_ARRAY)) {
try_array:
		SEPARATE_ARRAY(container);
fetch_from_array:
		if (dim == NULL) {
			retval = zend_hash_next_index_insert(Z_ARRVAL_P(container), &EG(uninitialized_zval));
			if (UNEXPECTED(retval == NULL)) {
				zend_error(E_WARNING, "Cannot add element to the array as the next element is already occupied");
				retval = &EG(error_zval);
			}
		} else {
			retval = php_taint_fetch_dimension_address_inner(Z_ARRVAL_P(container), dim, dim_type, type);
		}
		ZVAL_INDIRECT(result, retval);
		return;
	} else if (EXPECTED(Z_TYPE_P(container) == IS_REFERENCE)) {
		container = Z_REFVAL_P(container);
		if (EXPECTED(Z_TYPE_P(container) == IS_ARRAY)) {
			goto try_array;
		}
	}
	if (EXPECTED(Z_TYPE_P(container) == IS_STRING)) {
		if (type != BP_VAR_UNSET && UNEXPECTED(Z_STRLEN_P(container) == 0)) {
			zval_ptr_dtor_nogc(container);
convert_to_array:
			ZVAL_NEW_ARR(container);
			zend_hash_init(Z_ARRVAL_P(container), 8, NULL, ZVAL_PTR_DTOR, 0);
			goto fetch_from_array;
		}

		if (dim == NULL) {
			zend_throw_error(NULL, "[] operator not supported for strings");
			ZVAL_INDIRECT(result, &EG(error_zval));
		} else {
			php_taint_check_string_offset(dim, type);
			ZVAL_INDIRECT(result, NULL); /* wrong string offset */
		}
	} else if (EXPECTED(Z_TYPE_P(container) == IS_OBJECT)) {
		if (!Z_OBJ_HT_P(container)->read_dimension) {
			zend_throw_error(NULL, "Cannot use object as array");
			retval = &EG(error_zval);
		} else {
			retval = Z_OBJ_HT_P(container)->read_dimension(container, dim, type, result);

			if (UNEXPECTED(retval == &EG(uninitialized_zval))) {
				zend_class_entry *ce = Z_OBJCE_P(container);

				ZVAL_NULL(result);
				zend_error(E_NOTICE, "Indirect modification of overloaded element of %s has no effect", ZSTR_VAL(ce->name));
			} else if (EXPECTED(retval && Z_TYPE_P(retval) != IS_UNDEF)) {
				if (!Z_ISREF_P(retval)) {
					if (Z_REFCOUNTED_P(retval) &&
							Z_REFCOUNT_P(retval) > 1) {
						if (Z_TYPE_P(retval) != IS_OBJECT) {
							Z_DELREF_P(retval);
							ZVAL_DUP(result, retval);
							retval = result;
						} else {
							ZVAL_COPY_VALUE(result, retval);
							retval = result;
						}
					}
					if (Z_TYPE_P(retval) != IS_OBJECT) {
						zend_class_entry *ce = Z_OBJCE_P(container);
						zend_error(E_NOTICE, "Indirect modification of overloaded element of %s has no effect", ZSTR_VAL(ce->name));
					}
				} else if (UNEXPECTED(Z_REFCOUNT_P(retval) == 1)) {
					ZVAL_UNREF(retval);
				}
				if (result != retval) {
					ZVAL_INDIRECT(result, retval);
				}
			} else {
				ZVAL_INDIRECT(result, &EG(error_zval));
			}
		}
	} else if (EXPECTED(Z_TYPE_P(container) <= IS_FALSE)) {
		if (UNEXPECTED(container == &EG(error_zval))) {
			ZVAL_INDIRECT(result, &EG(error_zval));
		} else if (type != BP_VAR_UNSET) {
			goto convert_to_array;
		} else {
			/* for read-mode only */
			ZVAL_NULL(result);
		}
	} else {
		if (type == BP_VAR_UNSET) {
			zend_error(E_WARNING, "Cannot unset offset in a non-array variable");
			ZVAL_NULL(result);
		} else {
			zend_error(E_WARNING, "Cannot use a scalar value as an array");
			ZVAL_INDIRECT(result, &EG(error_zval));
		}
	}
}
/* }}} */

static void php_taint_assign_op_overloaded_property(zval *object, zval *property, void **cache_slot, zval *value, binary_op_type binary_op, zval *result) /* {{{ */ {
	zval *z;
	zval rv, obj;
	zval *zptr;
	int tainted = 0;

	ZVAL_OBJ(&obj, Z_OBJ_P(object));
	Z_ADDREF(obj);
	if (Z_OBJ_HT(obj)->read_property &&
		(z = Z_OBJ_HT(obj)->read_property(&obj, property, BP_VAR_R, cache_slot, &rv)) != NULL) {
		if (EG(exception)) {
			OBJ_RELEASE(Z_OBJ(obj));
			return;
		}
		if (Z_TYPE_P(z) == IS_OBJECT && Z_OBJ_HT_P(z)->get) {
			zval rv2;
			zval *value = Z_OBJ_HT_P(z)->get(z, &rv2);

			if (z == &rv) {
				zval_ptr_dtor(&rv);
			}
			ZVAL_COPY_VALUE(z, value);
		}
		zptr = z;
		ZVAL_DEREF(z);
		SEPARATE_ZVAL_NOREF(z);
		if (Z_TYPE_P(z) == IS_STRING && TAINT_POSSIBLE(Z_STR_P(z))) {
			tainted = 1;
		} else if (Z_TYPE_P(value) == IS_STRING && TAINT_POSSIBLE(Z_STR_P(value))) {
			tainted = 1;
		}
		binary_op(z, z, value);
		Z_OBJ_HT(obj)->write_property(&obj, property, z, cache_slot);
		if (result) {
			ZVAL_COPY(result, z);
		}
		if (tainted && Z_TYPE_P(z) == IS_STRING && Z_STRLEN_P(z)) {
			TAINT_MARK(Z_STR_P(z));
		}
		zval_ptr_dtor(zptr);
	} else {
		zend_error(E_WARNING, "Attempt to assign property of non-object");
		if (result) {
			ZVAL_NULL(result);
		}
	}
	OBJ_RELEASE(Z_OBJ(obj));
}
/* }}} */

static void php_taint_binary_assign_op_obj_dim(zval *object, zval *property, zval *value, zval *retval, binary_op_type binary_op) /* {{{ */ {
	zval *z;
	zval rv, res;
	int tainted = 0;

	if (Z_OBJ_HT_P(object)->read_dimension &&
		(z = Z_OBJ_HT_P(object)->read_dimension(object, property, BP_VAR_R, &rv)) != NULL) {

		if (Z_TYPE_P(z) == IS_OBJECT && Z_OBJ_HT_P(z)->get) {
			zval rv2;
			zval *value = Z_OBJ_HT_P(z)->get(z, &rv2);

			if (z == &rv) {
				zval_ptr_dtor(&rv);
			}
			ZVAL_COPY_VALUE(z, value);
		}
		if ((Z_TYPE_P(z) == IS_STRING && TAINT_POSSIBLE(Z_STR_P(z))) ||
			(Z_TYPE_P(z) == IS_REFERENCE && IS_STRING == Z_TYPE_P(Z_REFVAL_P(z)) &&
			 TAINT_POSSIBLE(Z_STR_P(Z_REFVAL_P(z))))) {
			tainted = 1;
		} else if (Z_TYPE_P(value) == IS_STRING && TAINT_POSSIBLE(Z_STR_P(value))) {
			tainted = 1;
		}

		binary_op(&res, Z_ISREF_P(z) ? Z_REFVAL_P(z) : z, value);
		Z_OBJ_HT_P(object)->write_dimension(object, property, &res);
		if (z == &rv) {
			zval_ptr_dtor(&rv);
		}
		if (retval) {
			ZVAL_COPY(retval, &res);
		}
		if (tainted && Z_TYPE(res) == IS_STRING && Z_STRLEN(res)) {
			TAINT_MARK(Z_STR(res));
		}
		zval_ptr_dtor(&res);
	} else {
		zend_error(E_WARNING, "Attempt to assign property of non-object");
		if (retval) {
			ZVAL_NULL(retval);
		}
	}
}
/* }}} */
/* Copied codes end */

static void php_taint_mark_strings(zend_array *symbol_table) /* {{{ */ {
	zval *val;
	ZEND_HASH_FOREACH_VAL(symbol_table, val) {
		if (Z_TYPE_P(val) == IS_ARRAY) {
			php_taint_mark_strings(Z_ARRVAL_P(val));
		} else if (IS_STRING == Z_TYPE_P(val) && Z_STRLEN_P(val)) {
			TAINT_MARK(Z_STR_P(val));
		}
	} ZEND_HASH_FOREACH_END();
} /* }}} */

static zval *php_taint_get_zval_ptr_tmpvar(zend_execute_data *execute_data, uint32_t var, zend_free_op *should_free) /* {{{ */ {
	zval *ret = EX_VAR(var);

	if (should_free) {
		*should_free = ret;
	}
	ZVAL_DEREF(ret);

	return ret;
}
/* }}} */	

#ifndef CV_DEF_OF
#define CV_DEF_OF(i) (EX(func)->op_array.vars[i])
#endif

static zval *php_taint_get_zval_ptr_cv(zend_execute_data *execute_data, uint32_t var, int type, int force_ret) /* {{{ */ {
	zval *ret = EX_VAR(var);

	if (UNEXPECTED(Z_TYPE_P(ret) == IS_UNDEF)) {
		if (force_ret) {
			switch (type) {
				case BP_VAR_R:
				case BP_VAR_UNSET:
					zend_error(E_NOTICE, "Undefined variable: %s", ZSTR_VAL(CV_DEF_OF(EX_VAR_TO_NUM(var))));
				case BP_VAR_IS:
					ret = &EG(uninitialized_zval);
					break;
				case BP_VAR_RW:
					zend_error(E_NOTICE, "Undefined variable: %s", ZSTR_VAL(CV_DEF_OF(EX_VAR_TO_NUM(var))));
				case BP_VAR_W:
					ZVAL_NULL(ret);
					break;
			}
		} else {
			return NULL;
		}
	} else {
		ZVAL_DEREF(ret);
	}
	return ret;
}
/* }}} */

static zval *php_taint_get_zval_ptr(zend_execute_data *execute_data, int op_type, znode_op op, taint_free_op *should_free, int type, int force_ret) /* {{{ */ {
	if (op_type & (IS_TMP_VAR|IS_VAR)) {
		return php_taint_get_zval_ptr_tmpvar(execute_data, op.var, should_free);
	} else {
		*should_free = NULL;
		if (op_type == IS_CONST) {
			return EX_CONSTANT(op);
		} else if (op_type == IS_CV) {
			return php_taint_get_zval_ptr_cv(execute_data, op.var, type, force_ret);
		} else {
			return NULL;
		}
	}
}
/* }}} */ 

static zval *php_taint_get_zval_ptr_ptr_var(zend_execute_data *execute_data, uint32_t var, zend_free_op *should_free) /* {{{ */ {
	zval *ret = EX_VAR(var);

	if (EXPECTED(Z_TYPE_P(ret) == IS_INDIRECT)) {
		*should_free = NULL;
		ret = Z_INDIRECT_P(ret);
	} else {
		*should_free = ret;
	}
	return ret;
}
/* }}} */

static zval *php_taint_get_zval_ptr_ptr(zend_execute_data *execute_data, int op_type, znode_op op, taint_free_op *should_free, int type) /* {{{ */ {
	if (op_type == IS_CV) {
		*should_free = NULL;
		return php_taint_get_zval_ptr_cv(execute_data, op.var, type, 1);
	} else if (op_type == IS_VAR) {
		ZEND_ASSERT(op_type == IS_VAR);
		return php_taint_get_zval_ptr_ptr_var(execute_data, op.var, should_free);
	} else if (op_type == IS_UNUSED) {
		*should_free = NULL;
		return &EX(This);
	} else {
		ZEND_ASSERT(0);
	}
}
/* }}} */

static void php_taint_error(const char *fname, const char *format, ...) /* {{{ */ {
	char *buffer, *msg;
	va_list args;

	va_start(args, format);
	vspprintf(&buffer, 0, format, args);
	spprintf(&msg, 0, "%s() [%s]: %s", get_active_function_name(), fname, buffer);
	efree(buffer);
	zend_error(TAINT_G(error_level), msg);
	efree(msg);
	va_end(args);
} /* }}} */

static int php_taint_echo_handler(zend_execute_data *execute_data) /* {{{ */ {
	const zend_op *opline = execute_data->opline;
	taint_free_op free_op1;
	zval *op1;

	op1 = php_taint_get_zval_ptr(execute_data, opline->op1_type, opline->op1, &free_op1, BP_VAR_R, 0);

	if (op1 && IS_STRING == Z_TYPE_P(op1) && TAINT_POSSIBLE(Z_STR_P(op1))) {
		if (opline->extended_value) {
			php_taint_error("print", "Attempt to print a string that might be tainted");
		} else {
			php_taint_error("echo", "Attempt to echo a string that might be tainted");
		}
	}

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int php_taint_exit_handler(zend_execute_data *execute_data) /* {{{ */ {
	const zend_op *opline = execute_data->opline;
	taint_free_op free_op1;
	zval *op1;

	op1 = php_taint_get_zval_ptr(execute_data, opline->op1_type, opline->op1, &free_op1, BP_VAR_R, 0);

	if (op1 && IS_STRING == Z_TYPE_P(op1) && TAINT_POSSIBLE(Z_STR_P(op1))) {
		php_taint_error("exit", "Attempt to output a string that might be tainted");
	}

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int php_taint_init_dynamic_fcall_handler(zend_execute_data *execute_data) /* {{{ */ {
	const zend_op *opline = execute_data->opline;
	taint_free_op free_op2;
	zval *op2;

	op2 = php_taint_get_zval_ptr(execute_data, opline->op2_type, opline->op2, &free_op2, BP_VAR_R, 0);

	if (op2) {
		if (IS_STRING == Z_TYPE_P(op2)) {
			if (TAINT_POSSIBLE(Z_STR_P(op2))) {
				php_taint_error("fcall", "Attempt to call a function which name might be tainted");
			}
		} else if (IS_ARRAY == Z_TYPE_P(op2)) {
			zval *cname = zend_hash_index_find(Z_ARRVAL_P(op2), 0);
			zval *mname = zend_hash_index_find(Z_ARRVAL_P(op2), 0);

			if (cname && IS_STRING == Z_TYPE_P(cname) && TAINT_POSSIBLE(Z_STR_P(cname))) {
				php_taint_error("fcall", "Attempt to call a method of a class which name might be tainted");
			} else if (mname && IS_STRING == Z_TYPE_P(mname) && TAINT_POSSIBLE(Z_STR_P(mname))) {
				php_taint_error("fcall", "Attempt to call a method which name might be tainted");
			}
		}
	}

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int php_taint_include_or_eval_handler(zend_execute_data *execute_data) /* {{{ */ {
	const zend_op *opline = execute_data->opline;
	taint_free_op free_op1;
	zval *op1;

	op1 = php_taint_get_zval_ptr(execute_data, opline->op1_type, opline->op1, &free_op1, BP_VAR_R, 0);

	if ((op1 && IS_STRING == Z_TYPE_P(op1) && TAINT_POSSIBLE(Z_STR_P(op1))))
		switch (opline->extended_value) {
			case ZEND_INCLUDE_ONCE:
				php_taint_error("include_once", "File path contains data that might be tainted");
				break;
			case ZEND_REQUIRE_ONCE:
				php_taint_error("require_once", "File path contains data that might be tainted");
				break;
			case ZEND_INCLUDE:
				php_taint_error("include", "File path contains data that might be tainted");
				break;
			case ZEND_REQUIRE:
				php_taint_error("require", "File path contains data that might be tainted");
				break;
			case ZEND_EVAL:
				php_taint_error("eval", "Code contains data that might be tainted");
				break;
		}

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static int php_taint_rope_handler(zend_execute_data *execute_data) /* {{{ */ {
	const zend_op *opline = execute_data->opline;
	zval *op2, *result;
	taint_free_op free_op2;
	zend_string **rope;
	char *target;
	int i, tainted = 0;
	size_t len = 0;

	rope = (zend_string **)EX_VAR(opline->op1.var);
	op2 = php_taint_get_zval_ptr(execute_data, opline->op2_type, opline->op2, &free_op2, BP_VAR_R, 1);
	result = EX_VAR(opline->result.var);

	rope[opline->extended_value] = zval_get_string(op2);

	for (i = 0; i <= opline->extended_value; i++) {
		if (TAINT_POSSIBLE(rope[i])) {
			tainted = 1;
		}
		len += ZSTR_LEN(rope[i]);
	}

	ZVAL_STR(result, zend_string_alloc(len, 0));
	target = Z_STRVAL_P(result);

	for (i = 0; i <= opline->extended_value; i++) {
		memcpy(target, ZSTR_VAL(rope[i]), ZSTR_LEN(rope[i]));
		target += ZSTR_LEN(rope[i]);
		zend_string_release(rope[i]);
	}
	*target = '\0';

	if (tainted) {
		TAINT_MARK(Z_STR_P(result));
	}

	execute_data->opline++;

	return ZEND_USER_OPCODE_CONTINUE;
} /* }}} */

static int php_taint_concat_handler(zend_execute_data *execute_data) /* {{{ */ {
	const zend_op *opline = execute_data->opline;
	zval *op1, *op2, *result;
	taint_free_op free_op1, free_op2;
	int tainted = 0;

	op1 = php_taint_get_zval_ptr(execute_data, opline->op1_type, opline->op1, &free_op1, BP_VAR_R, 1);
	op2 = php_taint_get_zval_ptr(execute_data, opline->op2_type, opline->op2, &free_op2, BP_VAR_R, 1);

	result = EX_VAR(opline->result.var);

	if ((op1 && IS_STRING == Z_TYPE_P(op1) && TAINT_POSSIBLE(Z_STR_P(op1)))
			|| (op2 && IS_STRING == Z_TYPE_P(op2) && TAINT_POSSIBLE(Z_STR_P(op2)))) {
		tainted = 1;
	}

	concat_function(result, op1, op2);

	if (tainted && IS_STRING == Z_TYPE_P(result) && Z_STRLEN_P(result)) {
		TAINT_MARK(Z_STR_P(result));
	}

	if ((TAINT_OP1_TYPE(opline) & (IS_VAR|IS_TMP_VAR)) && free_op1) {
		zval_ptr_dtor_nogc(free_op1);
	}

	if ((TAINT_OP2_TYPE(opline) & (IS_VAR|IS_TMP_VAR)) && free_op2) {
		zval_ptr_dtor_nogc(free_op2);
	}

	execute_data->opline++;

	return ZEND_USER_OPCODE_CONTINUE;
} /* }}} */

static int php_taint_binary_assign_op_helper(binary_op_type binary_op, zend_execute_data *execute_data) /* {{{ */ {
	const zend_op *opline = execute_data->opline;
	zval *var_ptr, *value;
	taint_free_op free_op1, free_op2;
	int tainted = 0;

	value = php_taint_get_zval_ptr(execute_data, opline->op2_type, opline->op2, &free_op2, BP_VAR_R, 1);
	var_ptr = php_taint_get_zval_ptr_ptr(execute_data, opline->op1_type, opline->op1, &free_op1, BP_VAR_RW);

	if (opline->op1_type == IS_VAR) {
		if (var_ptr == NULL || var_ptr == &EG(error_zval)) {
			return ZEND_USER_OPCODE_DISPATCH;
		}
	}

	if ((var_ptr && IS_STRING == Z_TYPE_P(var_ptr) && TAINT_POSSIBLE(Z_STR_P(var_ptr)))
			|| (value && IS_STRING == Z_TYPE_P(value) && TAINT_POSSIBLE(Z_STR_P(value)))) {
		tainted = 1;
	}

	SEPARATE_ZVAL_NOREF(var_ptr);

	binary_op(var_ptr, var_ptr, value);

	if (tainted && IS_STRING == Z_TYPE_P(var_ptr) && Z_STRLEN_P(var_ptr)) {
		TAINT_MARK(Z_STR_P(var_ptr));
	}

	if ((!((opline)->result_type & EXT_TYPE_UNUSED))) {
		ZVAL_COPY(EX_VAR(opline->result.var), var_ptr);
	}

	if ((TAINT_OP1_TYPE(opline) & (IS_VAR|IS_TMP_VAR)) && free_op1) {
		zval_ptr_dtor_nogc(free_op1);
	}

	if ((TAINT_OP2_TYPE(opline) & (IS_VAR|IS_TMP_VAR)) && free_op2) {
		zval_ptr_dtor_nogc(free_op2);
	}

	execute_data->opline++;

	return ZEND_USER_OPCODE_CONTINUE; 
} /* }}} */

static int php_taint_binary_assign_op_obj_helper(binary_op_type binary_op, zend_execute_data *execute_data) /* {{{ */ {
	const zend_op *opline = execute_data->opline;
	zval *object, *property, *var_ptr, *value;
	taint_free_op free_op1, free_op2, free_op_data;
	int tainted = 0;

	object = php_taint_get_zval_ptr_ptr(execute_data, opline->op1_type, opline->op1, &free_op1, BP_VAR_RW);
	if (opline->op1_type == IS_UNUSED && Z_OBJ_P(object) == NULL) {
		return ZEND_USER_OPCODE_DISPATCH;
	}
	if (opline->op1_type == IS_VAR && object == NULL) {
		return ZEND_USER_OPCODE_DISPATCH;
	}

	property = php_taint_get_zval_ptr(execute_data, opline->op2_type, opline->op2, &free_op2, BP_VAR_R, 1);

	do {
		if (opline->op1_type == IS_UNUSED || Z_TYPE_P(object) != IS_OBJECT) {
			if (!php_taint_make_real_object(object)) {
				zend_error(E_WARNING, "Attempt to assign property of non-object");
				if ((!((opline)->result_type & EXT_TYPE_UNUSED))) {
					ZVAL_NULL(EX_VAR(opline->result.var));
				}
				break;
			}
		}

		value = php_taint_get_zval_ptr(execute_data, (opline + 1)->op1_type, (opline + 1)->op1, &free_op_data, BP_VAR_R, 1);

		if (Z_OBJ_HT_P(object)->get_property_ptr_ptr
				&& (var_ptr = Z_OBJ_HT_P(object)->get_property_ptr_ptr(object, property, BP_VAR_RW, NULL)) != NULL) {
			ZVAL_DEREF(var_ptr);
			SEPARATE_ZVAL_NOREF(var_ptr);

			if (Z_TYPE_P(var_ptr) == IS_STRING && TAINT_POSSIBLE(Z_STR_P(var_ptr))) {
				tainted = 1;
			} else if (Z_TYPE_P(value) == IS_STRING && TAINT_POSSIBLE(Z_STR_P(value))) {
				tainted = 1;
			}

			binary_op(var_ptr, var_ptr, value);
			if ((!((opline)->result_type & EXT_TYPE_UNUSED))) {
				ZVAL_COPY(EX_VAR(opline->result.var), var_ptr);
			}

			if (tainted && Z_TYPE_P(var_ptr) == IS_STRING && Z_STRLEN_P(var_ptr)) {
				TAINT_MARK(Z_STR_P(var_ptr));
			}
		} else {
			php_taint_assign_op_overloaded_property(object, property, NULL, value, binary_op, EX_VAR(opline->result.var));
			if ((opline)->result_type & EXT_TYPE_UNUSED) {
				zval_ptr_dtor_nogc(EX_VAR(opline->result.var));
			}
		}
	} while (0);

	if ((opline->op2_type & (IS_VAR|IS_TMP_VAR)) && free_op2) {
		zval_ptr_dtor_nogc(free_op2);
	}
	if (((opline + 1)->op1_type & (IS_VAR|IS_TMP_VAR)) && free_op_data)   {
		zval_ptr_dtor_nogc(free_op_data);
	}
	if ((opline->op1_type & (IS_VAR|IS_TMP_VAR)) && free_op1) {
		zval_ptr_dtor_nogc(free_op1);
	}
	execute_data->opline += 2;

	return ZEND_USER_OPCODE_CONTINUE;
}
/* }}} */

static int php_taint_binary_assign_op_dim_helper(binary_op_type binary_op, zend_execute_data *execute_data) /* {{{ */ {
	const zend_op *opline = execute_data->opline;
	zval *container, *dim, *var_ptr, *value, rv;
	taint_free_op free_op1, free_op2, free_op_data;
	int tainted = 0;

	container = php_taint_get_zval_ptr_ptr(execute_data, opline->op1_type, opline->op1, &free_op1, BP_VAR_RW);
	if (opline->op1_type == IS_UNUSED && Z_OBJ_P(container) == NULL) {
		return ZEND_USER_OPCODE_DISPATCH;
	}
	if (opline->op1_type == IS_VAR && container == NULL) {
		return ZEND_USER_OPCODE_DISPATCH;
	}

	dim = php_taint_get_zval_ptr(execute_data, opline->op2_type, opline->op2, &free_op2, BP_VAR_R, 1);

	do {
		if (opline->op1_type == IS_UNUSED || Z_TYPE_P(container) == IS_OBJECT) {
			value = php_taint_get_zval_ptr(execute_data, (opline + 1)->op1_type, (opline + 1)->op1, &free_op_data, BP_VAR_R, 1);
			php_taint_binary_assign_op_obj_dim(container, dim, value, EX_VAR(opline->result.var), binary_op);
			
			if ((opline)->result_type & EXT_TYPE_UNUSED) {
				zval_ptr_dtor_nogc(EX_VAR(opline->result.var));
			}
			break;
		}

		php_taint_fetch_dimension_address(&rv, container, dim, opline->op2_type, BP_VAR_RW);
		value = php_taint_get_zval_ptr(execute_data, (opline + 1)->op1_type, (opline + 1)->op1, &free_op_data, BP_VAR_R, 1);
		ZEND_ASSERT(Z_TYPE(rv) == IS_INDIRECT);
		var_ptr = Z_INDIRECT(rv);

		if (var_ptr == NULL) {
			zend_throw_error(NULL, "Cannot use assign-op operators with overloaded objects nor string offsets");
			if ((opline->op2_type & (IS_VAR|IS_TMP_VAR)) && free_op2) {
				zval_ptr_dtor_nogc(free_op2);
			}
			if (((opline + 1)->op1_type & (IS_VAR|IS_TMP_VAR)) && free_op_data)   {
				zval_ptr_dtor_nogc(free_op_data);
			}
			if ((opline->op1_type & (IS_VAR|IS_TMP_VAR)) && free_op1) {
				zval_ptr_dtor_nogc(free_op1);
			}
			execute_data->opline += 2;
			return ZEND_USER_OPCODE_CONTINUE;
		}

		if (var_ptr == &EG(error_zval)) {
			if ((!((opline)->result_type & EXT_TYPE_UNUSED))) {
				ZVAL_NULL(EX_VAR(opline->result.var));
			}
		} else {
			ZVAL_DEREF(var_ptr);
			SEPARATE_ZVAL_NOREF(var_ptr);

			if (Z_TYPE_P(var_ptr) == IS_STRING && TAINT_POSSIBLE(Z_STR_P(var_ptr))) {
				tainted = 1;
			} else if (Z_TYPE_P(value) == IS_STRING && TAINT_POSSIBLE(Z_STR_P(value))) {
				tainted = 1;
			}

			binary_op(var_ptr, var_ptr, value);

			if ((!((opline)->result_type & EXT_TYPE_UNUSED))) {
				ZVAL_COPY(EX_VAR(opline->result.var), var_ptr);
			}

			if (tainted && Z_TYPE_P(var_ptr) == IS_STRING && Z_STRLEN_P(var_ptr)) {
				TAINT_MARK(Z_STR_P(var_ptr));
			}
		}
	} while (0);

	if ((opline->op2_type & (IS_VAR|IS_TMP_VAR)) && free_op2) {
		zval_ptr_dtor_nogc(free_op2);
	}
	if (((opline + 1)->op1_type & (IS_VAR|IS_TMP_VAR)) && free_op_data)   {
		zval_ptr_dtor_nogc(free_op_data);
	}
	if ((opline->op1_type & (IS_VAR|IS_TMP_VAR)) && free_op1) {
		zval_ptr_dtor_nogc(free_op1);
	}
	execute_data->opline += 2;

	return ZEND_USER_OPCODE_CONTINUE;
}
/* }}} */

static int php_taint_assign_concat_handler(zend_execute_data *execute_data) /* {{{ */ {
	const zend_op *opline = execute_data->opline;

	if (EXPECTED(opline->extended_value == 0)) {
		return php_taint_binary_assign_op_helper(concat_function, execute_data);
	} else if (EXPECTED(opline->extended_value == ZEND_ASSIGN_DIM)) {
		return php_taint_binary_assign_op_dim_helper(concat_function, execute_data);
	} else {
		return php_taint_binary_assign_op_obj_helper(concat_function, execute_data);
	}
} /* }}} */

static void php_taint_fcall_check(zend_execute_data *ex, const zend_op *opline, zend_function *fbc) /* {{{ */ {
	int arg_count = ZEND_CALL_NUM_ARGS(ex);

	if (!arg_count) {
		return;
	}

	if (fbc->common.scope == NULL) {
		do {
			const char *fname = ZSTR_VAL(fbc->common.function_name);
			size_t len = ZSTR_LEN(fbc->common.function_name) + 1; /* include the tail zero */
			if (strncmp("print_r", fname, len) == 0) {
				zval *p = ZEND_CALL_ARG(ex, 1);
				if (p && IS_STRING == Z_TYPE_P(p) && TAINT_POSSIBLE(Z_STR_P(p))) {
					php_taint_error(fname, "Attempt to print_r data that might be tainted");
				}
				break;
			}

			if (strncmp("fopen", fname, len) == 0) {
				zval *p = ZEND_CALL_ARG(ex, 1);
				if (p && IS_STRING == Z_TYPE_P(p) && TAINT_POSSIBLE(Z_STR_P(p))) {
					php_taint_error(fname, "Attempt to open a file which path might be tainted");
				}
				break;
			}

			if (strncmp("file", fname, len) == 0
				|| strncmp("readfile", fname, len) == 0
				|| strncmp("file_get_contents", fname, len) == 0) {
				zval *p = ZEND_CALL_ARG(ex, 1);
				if (p && IS_STRING == Z_TYPE_P(p) && TAINT_POSSIBLE(Z_STR_P(p))) {
					php_taint_error(fname, "Attempt to read a file which path might be tainted");
				}
				break;
			}

			if (strncmp("opendir", fname, len) == 0) {
				zval *p = ZEND_CALL_ARG(ex, 1);
				if (p && IS_STRING == Z_TYPE_P(p) && TAINT_POSSIBLE(Z_STR_P(p))) {
					php_taint_error(fname, "Attempt to open a directory which path might be tainted");
				}
				break;
			}

			if (strncmp("printf", fname, len) == 0) {
				if (arg_count > 1) {
					uint32_t i;
					for (i = 0; i < arg_count; i++) {
						zval *p = ZEND_CALL_ARG(ex, i + 1);
						if (p && IS_STRING == Z_TYPE_P(p) && TAINT_POSSIBLE(Z_STR_P(p))) {
							php_taint_error(fname, "%dth argument contains data that might be tainted", i + 1);
							break;
						}
					}
				}
				break;
			}

			if (strncmp("vprintf", fname, len) == 0) {
				if (arg_count > 1) {
					zend_string *key;
					zend_long idx;
					zval *val, *p = ZEND_CALL_ARG(ex, 1);
					if (IS_ARRAY != Z_TYPE_P(p) || zend_hash_num_elements(Z_ARRVAL_P(p))) {
						break;
					}

					ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(p), idx, key, val) {
						if (IS_STRING == Z_TYPE_P(val) && TAINT_POSSIBLE(Z_STR_P(val))) {
							if (key) {
								php_taint_error(fname, "Second argument contains data(index:%s) that might be tainted", ZSTR_VAL(key));
							} else {
								php_taint_error(fname, "Second argument contains data(index:%ld) that might be tainted", idx);
							}
							break;
						}
					} ZEND_HASH_FOREACH_END();
				}
				break;
			}

			if (strncmp("file_put_contents", fname, len) == 0
				|| strncmp("fwrite", fname, len) == 0) {
				if (arg_count > 1) {
					zval *fp, *str;

					fp = ZEND_CALL_ARG(ex, 1);
					str = ZEND_CALL_ARG(ex, 2);

					if (IS_RESOURCE == Z_TYPE_P(fp)) {
						break;
					} else if (IS_STRING == Z_TYPE_P(fp)) {
						if (strncasecmp("php://output", Z_STRVAL_P(fp), Z_STRLEN_P(fp))) {
							break;
						}
					}
					if (IS_STRING == Z_TYPE_P(str) && TAINT_POSSIBLE(Z_STR_P(str))) {
						php_taint_error(fname, "Attempt to output data that might be tainted");
					}
				}
				break;
			}

			if (strncmp("header", fname, len) == 0) {
				zval *header = ZEND_CALL_ARG(ex, 1);
				if (IS_STRING == Z_TYPE_P(header) && TAINT_POSSIBLE(Z_STR_P(header))) {
					php_taint_error(fname, "Attempt to send a header that might be tainted");
				}
				break;
			}

			if (strncmp("unserialize", fname, len) == 0) {
				/* TODO: allow_classes? */
				zval *str = ZEND_CALL_ARG(ex, 1);
				if (IS_STRING == Z_TYPE_P(str) && TAINT_POSSIBLE(Z_STR_P(str))) {
					php_taint_error(fname, "Attempt to unserialize a string that might be tainted");
				}
				break;
			}

			if (strncmp("mysqli_query", fname, len) == 0
					|| strncmp("mysql_query", fname, len) == 0
					|| strncmp("sqlite_query", fname, len) == 0
					|| strncmp("sqlite_single_query", fname, len) == 0 ) {
				zval *query = ZEND_CALL_ARG(ex, arg_count);
				if (IS_STRING == Z_TYPE_P(query) && TAINT_POSSIBLE(Z_STR_P(query))) {
					php_taint_error(fname, "SQL statement contains data that might be tainted");
				}
				break;
			}

			if (strncmp("oci_parse", fname, len) == 0) {
				if (arg_count > 1) {
					zval *sql = ZEND_CALL_ARG(ex, 2);
					if (IS_STRING == Z_TYPE_P(sql) && TAINT_POSSIBLE(Z_STR_P(sql))) {
						php_taint_error(fname, "SQL statement contains data that might be tainted");
					}
				}
				break;
			}

			if (strncmp("preg_replace_callback", fname, len) == 0) {
				if (arg_count > 1) {
					zval *callback = ZEND_CALL_ARG(ex, 2);
					if (IS_STRING == Z_TYPE_P(callback)) {
						if (TAINT_POSSIBLE(Z_STR_P(callback))) {
							php_taint_error(fname, "Callback name contains data that might be tainted");
						}
					} else if (IS_ARRAY == Z_TYPE_P(callback)) {
						zval *cname = zend_hash_index_find(Z_ARRVAL_P(callback), 0);
						zval *mname = zend_hash_index_find(Z_ARRVAL_P(callback), 0);

						if (cname && IS_STRING == Z_TYPE_P(cname) && TAINT_POSSIBLE(Z_STR_P(cname))) {
							php_taint_error(fname, "Callback class name contains data that might be tainted");
						} else if (mname && IS_STRING == Z_TYPE_P(mname) && TAINT_POSSIBLE(Z_STR_P(mname))) {
							php_taint_error(fname, "Callback method name contains data that might be tainted");
						}
					}
				}
				break;
			}

			if (strncmp("passthru", fname, len) == 0
				|| strncmp("system", fname, len) == 0
				|| strncmp("exec", fname, len) == 0
				|| strncmp("shell_exec", fname, len) == 0
				|| strncmp("proc_open", fname, len) == 0 
				|| strncmp("popen", fname, len) == 0) {
				zval *cmd = ZEND_CALL_ARG(ex, arg_count);
				if (IS_STRING == Z_TYPE_P(cmd) && TAINT_POSSIBLE(Z_STR_P(cmd))) {
					php_taint_error(fname, "CMD statement contains data that might be tainted");
				}
				break;
			}

		} while (0);
	} else {
		do {
			const char *class_name = ZSTR_VAL(fbc->common.scope->name);
			size_t cname_len = ZSTR_LEN(fbc->common.scope->name) + 1;
			const char *fname = ZSTR_VAL(fbc->common.function_name);
			size_t len = ZSTR_LEN(fbc->common.function_name) + 1; /* include the tail zero */

			if (strncmp("mysqli", class_name, cname_len) == 0) {
				if (strncmp("query", fname, len) == 0) {
					zval *sql = ZEND_CALL_ARG(ex, arg_count);
					if (IS_STRING == Z_TYPE_P(sql) && TAINT_POSSIBLE(Z_STR_P(sql))) {
						php_taint_error(fname, "SQL statement contains data that might be tainted");
					}
				}
				break;
			}

			if (strncmp("sqlitedatabase", class_name, cname_len) == 0) {
				if (strncmp("query", fname, len) == 0
						|| strncmp("singlequery", fname, len) == 0) {
					zval *sql = ZEND_CALL_ARG(ex, arg_count);
					if (IS_STRING == Z_TYPE_P(sql) && TAINT_POSSIBLE(Z_STR_P(sql))) {
						php_taint_error(fname, "SQL statement contains data that might be tainted");
					}
				}
				break;
			}

			if (strncmp("pdo", class_name, cname_len) == 0) {
				if (strncmp("query", fname, len) == 0
					|| strncmp("prepare", fname, len) == 0) {
					zval *sql = ZEND_CALL_ARG(ex, arg_count);
					if (IS_STRING == Z_TYPE_P(sql) && TAINT_POSSIBLE(Z_STR_P(sql))) {
						php_taint_error(fname, "SQL statement contains data that might be tainted");
					}
				}
				break;
			}
		} while (0);
	}
} /* }}} */

static int php_taint_fcall_handler(zend_execute_data *execute_data) /* {{{ */ {
	const zend_op *opline = execute_data->opline;
	zend_execute_data *call = execute_data->call;
	zend_function *fbc = call->func;

	if (fbc->type == ZEND_INTERNAL_FUNCTION) {
		php_taint_fcall_check(call, opline, fbc);
	}

	return ZEND_USER_OPCODE_DISPATCH;
} /* }}} */

static void php_taint_register_handlers() /* {{{ */ {
	zend_set_user_opcode_handler(ZEND_ECHO, php_taint_echo_handler);
	zend_set_user_opcode_handler(ZEND_EXIT, php_taint_exit_handler);
	zend_set_user_opcode_handler(ZEND_INIT_USER_CALL, php_taint_init_dynamic_fcall_handler);
	zend_set_user_opcode_handler(ZEND_INIT_DYNAMIC_CALL, php_taint_init_dynamic_fcall_handler);
	zend_set_user_opcode_handler(ZEND_INCLUDE_OR_EVAL, php_taint_include_or_eval_handler);
	zend_set_user_opcode_handler(ZEND_CONCAT, php_taint_concat_handler);
	zend_set_user_opcode_handler(ZEND_FAST_CONCAT, php_taint_concat_handler);
	zend_set_user_opcode_handler(ZEND_ASSIGN_CONCAT, php_taint_assign_concat_handler);
	zend_set_user_opcode_handler(ZEND_ROPE_END, php_taint_rope_handler);
	zend_set_user_opcode_handler(ZEND_DO_FCALL, php_taint_fcall_handler);
	zend_set_user_opcode_handler(ZEND_DO_ICALL, php_taint_fcall_handler);
	zend_set_user_opcode_handler(ZEND_DO_FCALL_BY_NAME, php_taint_fcall_handler);
} /* }}} */

static void php_taint_override_func(const char *name, php_func handler, php_func *stash) /* {{{ */ {
	zend_function *func;
	if ((func = zend_hash_str_find_ptr(CG(function_table), name, strlen(name))) != NULL) {
		if (stash) {
			*stash = func->internal_function.handler;
		}
		func->internal_function.handler = handler;
	}
} /* }}} */

static void php_taint_override_functions() /* {{{ */ {
	const char *f_join        = "join";
	const char *f_trim        = "trim";
	const char *f_split       = "split";
	const char *f_rtrim       = "rtrim";
	const char *f_ltrim       = "ltrim";
	const char *f_strval      = "strval";
	const char *f_strstr      = "strstr";
	const char *f_substr      = "substr";
	const char *f_sprintf     = "sprintf";
	const char *f_explode     = "explode";
	const char *f_implode     = "implode";
	const char *f_str_pad     = "str_pad";
	const char *f_vsprintf    = "vsprintf";
	const char *f_str_replace = "str_replace";
	const char *f_strtolower  = "strtolower";
	const char *f_strtoupper  = "strtoupper";
	const char *f_dirname     = "dirname";
	const char *f_basename    = "basename";
	const char *f_pathinfo    = "pathinfo";

	php_taint_override_func(f_strval, PHP_FN(taint_strval), &TAINT_O_FUNC(strval));
	php_taint_override_func(f_sprintf, PHP_FN(taint_sprintf), &TAINT_O_FUNC(sprintf));
	php_taint_override_func(f_vsprintf, PHP_FN(taint_vsprintf), &TAINT_O_FUNC(vsprintf));
	php_taint_override_func(f_explode, PHP_FN(taint_explode), &TAINT_O_FUNC(explode));
	php_taint_override_func(f_split, PHP_FN(taint_explode), NULL);
	php_taint_override_func(f_implode, PHP_FN(taint_implode), &TAINT_O_FUNC(implode));
	php_taint_override_func(f_join, PHP_FN(taint_implode), NULL);
	php_taint_override_func(f_trim, PHP_FN(taint_trim), &TAINT_O_FUNC(trim));
	php_taint_override_func(f_rtrim, PHP_FN(taint_rtrim), &TAINT_O_FUNC(rtrim));
	php_taint_override_func(f_ltrim, PHP_FN(taint_ltrim), &TAINT_O_FUNC(ltrim));
	php_taint_override_func(f_str_replace, PHP_FN(taint_str_replace), &TAINT_O_FUNC(str_replace));
	php_taint_override_func(f_str_pad, PHP_FN(taint_str_pad), &TAINT_O_FUNC(str_pad));
	php_taint_override_func(f_strstr, PHP_FN(taint_strstr), &TAINT_O_FUNC(strstr));
	php_taint_override_func(f_strtolower, PHP_FN(taint_strtolower), &TAINT_O_FUNC(strtolower));
	php_taint_override_func(f_strtoupper, PHP_FN(taint_strtoupper), &TAINT_O_FUNC(strtoupper));
	php_taint_override_func(f_substr, PHP_FN(taint_substr), &TAINT_O_FUNC(substr));
	php_taint_override_func(f_dirname, PHP_FN(taint_dirname), &TAINT_O_FUNC(dirname));
	php_taint_override_func(f_basename, PHP_FN(taint_basename), &TAINT_O_FUNC(basename));
	php_taint_override_func(f_pathinfo, PHP_FN(taint_pathinfo), &TAINT_O_FUNC(pathinfo));

} /* }}} */

#ifdef COMPILE_DL_TAINT
ZEND_GET_MODULE(taint)
#endif

/* {{{ proto string strval(mixed $value)
*/
PHP_FUNCTION(taint_strval) {
	zval *num;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &num) == FAILURE) {
		return;
	}

	if (Z_TYPE_P(num) == IS_STRING && TAINT_POSSIBLE(Z_STR_P(num))) {
		tainted = 1;
	}

	TAINT_O_FUNC(strval)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) 
			&& Z_STR_P(return_value) != Z_STR_P(num) && Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string sprintf(string $format, ...)
*/
PHP_FUNCTION(taint_sprintf) {
	zval *args;
	int i, argc, tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "+", &args, &argc) == FAILURE) {
		RETURN_FALSE;
	}

	for (i = 0; i < argc; i++) {
		if (IS_STRING == Z_TYPE(args[i]) && TAINT_POSSIBLE(Z_STR(args[i]))) {
			tainted = 1;
			break;
		}
	}

	TAINT_O_FUNC(sprintf)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string vsprintf(string $format, ...)
*/
PHP_FUNCTION(taint_vsprintf) {
	zval *args;
	zend_string *format;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "Sa", &format, &args) == FAILURE) {
		RETURN_FALSE;
	}

	do {
		zval *val;
		if (TAINT_POSSIBLE(format)) {
			tainted = 1;
			break;
		}

		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(args), val) {
			if (IS_STRING == Z_TYPE_P(val) && TAINT_POSSIBLE(Z_STR_P(val))) {
				tainted = 1;
				break;
			}
		} ZEND_HASH_FOREACH_END();
	} while (0);

	TAINT_O_FUNC(vsprintf)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto array explode(string $separator, string $str[, int $limit])
*/
PHP_FUNCTION(taint_explode) {
	zend_string *str, *delim;
	zend_long limit = ZEND_LONG_MAX;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "SS|l", &delim, &str, &limit) == FAILURE) {
		return;
	}

	if (TAINT_POSSIBLE(str)) {
		tainted = 1;
	}

	TAINT_O_FUNC(explode)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_ARRAY == Z_TYPE_P(return_value) && zend_hash_num_elements(Z_ARRVAL_P(return_value))) {
		php_taint_mark_strings(Z_ARRVAL_P(return_value));
	}
}
/* }}} */

/* {{{ proto string implode(string $separator, array $args)
*/
PHP_FUNCTION(taint_implode) {
	zval *op1, *op2;
	zval *target = NULL;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "zz", &op1, &op2) == FAILURE) {
		ZVAL_FALSE(return_value);
		WRONG_PARAM_COUNT;
	}

	if (IS_ARRAY == Z_TYPE_P(op1)) {
		target = op1;
	} else if(IS_ARRAY == Z_TYPE_P(op2)) {
		target = op2;
	}

	if (target) {
		zval *val;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(target), val) {
			if (IS_STRING == Z_TYPE_P(val) && Z_STRLEN_P(val) && TAINT_POSSIBLE(Z_STR_P(val))) {
				tainted = 1;
				break;
			}
		} ZEND_HASH_FOREACH_END();
	}

	TAINT_O_FUNC(implode)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string trim(string $str)
*/
PHP_FUNCTION(taint_trim)
{
	zend_string *str, *what;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S|S", &str, &what) == FAILURE) {
		return;
	}

	if (TAINT_POSSIBLE(str)) {
		tainted = 1;
	}

	TAINT_O_FUNC(trim)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) && 
			Z_STR_P(return_value) != str && Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string rtrim(string $str)
*/
PHP_FUNCTION(taint_rtrim)
{
	PHP_FN(taint_trim)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

/* {{{ proto string ltrim(string $str)
*/
PHP_FUNCTION(taint_ltrim)
{
	PHP_FN(taint_trim)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

/* {{{ proto string str_replace(mixed $search, mixed $replace, mixed $subject [, int &$count])
*/
PHP_FUNCTION(taint_str_replace)
{
	zval *str, *from, *len, *repl;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "zzz|z", &str, &repl, &from, &len) == FAILURE) {
		return;
	}

	if (IS_STRING == Z_TYPE_P(repl) && TAINT_POSSIBLE(Z_STR_P(repl))) {
		tainted = 1;
	} else if (IS_STRING == Z_TYPE_P(from) && TAINT_POSSIBLE(Z_STR_P(from))) {
		tainted = 1;
	}

	TAINT_O_FUNC(str_replace)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string str_pad(string $input, int $pad_length[, string $pad_string = " "[, int $pad_type = STR_PAD_RIGHT]])
*/
PHP_FUNCTION(taint_str_pad)
{
	zend_string *input;
	zend_long pad_length;
	zend_string *pad_str = NULL;
	zend_long pad_type_val = 1;
	int	tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "Sl|Sl", &input, &pad_length, &pad_str, &pad_type_val) == FAILURE) {
		return;
	}

	if (TAINT_POSSIBLE(input)) {
		tainted = 1;
	} else if (pad_str && TAINT_POSSIBLE(pad_str)) {
		tainted = 1;
	}

	TAINT_O_FUNC(str_pad)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) && Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string strstr(string $haystack, mixed $needle[, bool $part = false])
*/
PHP_FUNCTION(taint_strstr)
{
	zval *needle;
	zend_string *haystack;
	zend_bool part = 0;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "Sz|b", &haystack, &needle, &part) == FAILURE) {
		return;
	}

	if (TAINT_POSSIBLE(haystack)) {
		tainted = 1;
	}

	TAINT_O_FUNC(strstr)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) &&
			Z_STR_P(return_value) != haystack &&	Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string substr(string $string, int $start[, int $length])
*/
PHP_FUNCTION(taint_substr)
{
	zend_string *str;
	zend_long l = 0, f;
	int	tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "Sl|l", &str, &f, &l) == FAILURE) {
		return;
	}

	if (TAINT_POSSIBLE(str)) {
		tainted = 1;
	}

	TAINT_O_FUNC(substr)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) && 
			Z_STR_P(return_value) != str && Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string strtolower(string $string)
*/
PHP_FUNCTION(taint_strtolower)
{
	zend_string *str;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S", &str) == FAILURE) {
		return;
	}

	if (TAINT_POSSIBLE(str)) {
		tainted = 1;
	}

	TAINT_O_FUNC(strtolower)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) &&
			Z_STR_P(return_value) != str	&& Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string strtoupper(string $string)
*/
PHP_FUNCTION(taint_strtoupper)
{
	zend_string *str;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S", &str) == FAILURE) {
		return;
	}

	if (TAINT_POSSIBLE(str)) {
		tainted = 1;
	}

	TAINT_O_FUNC(strtoupper)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) &&
			Z_STR_P(return_value) != str && Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string dirname(string $path, int level)
*/
PHP_FUNCTION(taint_dirname) {
	zend_string *str;
	zend_long levels = 1;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S|l", &str, &levels) == FAILURE) {
		return;
	}

	if (TAINT_POSSIBLE(str)) {
		tainted = 1;
	}

	TAINT_O_FUNC(dirname)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) 
			&& Z_STR_P(return_value) != str && Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string basename(string $path[, string $suffix])
*/
PHP_FUNCTION(taint_basename) {
	zend_string *string, *suffix = NULL;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S|S", &string, &suffix) == FAILURE) {
		return;
	}

	if (TAINT_POSSIBLE(string)) {
		tainted = 1;
	}

	TAINT_O_FUNC(basename)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted && IS_STRING == Z_TYPE_P(return_value) 
			&& Z_STR_P(return_value) != string && Z_STRLEN_P(return_value)) {
		TAINT_MARK(Z_STR_P(return_value));
	}
}
/* }}} */

/* {{{ proto string pathinfo(string $path[, int $options])
*/
PHP_FUNCTION(taint_pathinfo) {
	zend_string *path;
	zend_long opt;
	int tainted = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S|l", &path, &opt) == FAILURE) {
		return;
	}

	if (TAINT_POSSIBLE(path)) {
		tainted = 1;
	}

	TAINT_O_FUNC(pathinfo)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (tainted) {
		if (IS_STRING == Z_TYPE_P(return_value)) {
			if (Z_STR_P(return_value) != path && Z_STRLEN_P(return_value)) {
				TAINT_MARK(Z_STR_P(return_value));
			}
		} else if (IS_ARRAY == Z_TYPE_P(return_value)) {
			php_taint_mark_strings(Z_ARRVAL_P(return_value));
		}
	}
}
/* }}} */

static PHP_INI_MH(OnUpdateErrorLevel) /* {{{ */ {
	if (!new_value) {
		TAINT_G(error_level) = E_USER_WARNING;
	} else {
		TAINT_G(error_level) = (int)atoi(ZSTR_VAL(new_value));
	}
	return SUCCESS;
} /* }}} */

/* {{{ PHP_INI
*/
PHP_INI_BEGIN()
	STD_PHP_INI_BOOLEAN("taint.enable", "0", PHP_INI_SYSTEM, OnUpdateBool, enable, zend_taint_globals, taint_globals)
	STD_PHP_INI_ENTRY("taint.error_level", "512", PHP_INI_ALL, OnUpdateErrorLevel, error_level, zend_taint_globals, taint_globals)
PHP_INI_END()
	/* }}} */

	/* {{{ proto bool taint(string $str[, string ...])
	*/
PHP_FUNCTION(taint)
{
	zval *args;
	int argc;
	int i;

	if (!TAINT_G(enable)) {
		RETURN_TRUE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "+", &args, &argc) == FAILURE) {
		return;
	}

	for (i = 0; i < argc; i++) {
		zval *el = &args[i];
		ZVAL_DEREF(el);
		if (IS_STRING == Z_TYPE_P(el) && Z_STRLEN_P(el) && !TAINT_POSSIBLE(Z_STR_P(el))) {
			TAINT_MARK(Z_STR_P(el));
		}
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool untaint(string $str[, string ...])
*/
PHP_FUNCTION(untaint)
{
	zval *args;
	int argc;
	int i;

	if (!TAINT_G(enable)) {
		RETURN_TRUE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "+", &args, &argc) == FAILURE) {
		return;
	}

	for (i = 0; i < argc; i++) {
		zval *el = &args[i];
		ZVAL_DEREF(el);
		if (IS_STRING == Z_TYPE_P(el) && TAINT_POSSIBLE(Z_STR_P(el))) {
			TAINT_CLEAN(Z_STR_P(el));
		}
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool is_tainted(string $str)
*/
PHP_FUNCTION(is_tainted)
{
	zval *arg;

	if (!TAINT_G(enable)) {
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &arg) == FAILURE) {
		return;
	}

	ZVAL_DEREF(arg);
	if (IS_STRING == Z_TYPE_P(arg) && TAINT_POSSIBLE(Z_STR_P(arg))) {
		RETURN_TRUE;
	}

	RETURN_FALSE;
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
*/
PHP_MINIT_FUNCTION(taint)
{
	REGISTER_INI_ENTRIES();

	if (!TAINT_G(enable)) {
		return SUCCESS;
	}

	php_taint_register_handlers();
	php_taint_override_functions();

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
*/
PHP_MSHUTDOWN_FUNCTION(taint)
{
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION
*/
PHP_RINIT_FUNCTION(taint)
{
	if (SG(sapi_started) || !TAINT_G(enable)) {
		return SUCCESS;
	}

	if (Z_TYPE(PG(http_globals)[TRACK_VARS_POST]) == IS_ARRAY) {
		php_taint_mark_strings(Z_ARRVAL(PG(http_globals)[TRACK_VARS_POST]));
	}

	if (Z_TYPE(PG(http_globals)[TRACK_VARS_GET]) == IS_ARRAY) {
		php_taint_mark_strings(Z_ARRVAL(PG(http_globals)[TRACK_VARS_GET]));
	}

	if (Z_TYPE(PG(http_globals)[TRACK_VARS_COOKIE]) == IS_ARRAY) {
		php_taint_mark_strings(Z_ARRVAL(PG(http_globals)[TRACK_VARS_COOKIE]));
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION
*/
PHP_RSHUTDOWN_FUNCTION(taint)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
*/
PHP_MINFO_FUNCTION(taint)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "taint support", "enabled");
	php_info_print_table_row(2, "Version", PHP_TAINT_VERSION);
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
